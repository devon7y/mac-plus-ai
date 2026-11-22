/*
 * Shuttle: the session with the server and the transfers (../PROTOCOL.md).
 *
 * Commands go out in C frames with a command number and are sent again (same number) when no reply
 * comes; replies come back in R frames. Listings and downloads arrive as data blocks with a sliding
 * window paced by this side: blocks are collected in a 32 KB buffer and acked while the buffer has
 * room for the blocks that may still be in flight. When it is full, acks stop, the blocks in flight
 * arrive, the line goes quiet, and only then is the buffer written to disk (the Plus's floppy driver
 * masks interrupts while it writes, so nothing may arrive meanwhile), then the ack lets the server go
 * on. Uploads work the other way round: 32 KB is read from disk only when every block sent before has
 * been acked. The same logic, in Python, is tested against the server in server/test_server.py.
 */
#include <OSUtils.h>
#include <Events.h>
#include <MacMemory.h>
#include <ToolUtils.h>
#include <Quickdraw.h>
#include <string.h>
#include "serial.h"
#include "link.h"
#include "proto.h"
#include "files.h"

short pState, pJob;
long pDone, pTotal;
unsigned long pStart, pEnd;
char serverName[32];
short pWindow = 4, pBlock = 1024;
long stNaks, stTimeouts, stFlushes, stResends, stBlocks, stLateFrames;
short dlVerifyFailed;

static Ptr xbuf;                     /* the 32 KB transfer buffer */
static long xbufSize;
static Ptr listBuf;                  /* a listing being received */
static long listUsed;

/* commands */
static unsigned short cmdNo;
static char cmdText[300];
static short cmdLen, cmdWaiting, cmdTries;
static unsigned long cmdTick, greetTick;
static short cancelledJob;

/* receiving */
static unsigned char tid;
static long rSize, rN, rNext, rGranted, rSeen, rNakSeen, rLastNakVal, rUsed;
static unsigned long rLastNakTick, rProgressTick, doneTick;
static short rHolding, rTries, doneTries;
static long expDlen, expRlen;

/* sending */
static long sTotal, sN, sBase, sNxt, sChunkStart, sChunkEnd;
static unsigned long sProgressTick, saveTick;
static short sTries, saveTries;

static unsigned long nakWait(void)   /* ticks: W blocks at the line speed, plus 1 s */
{
    long bps = ser_lineBps(prefs.baud);
    if (bps < 50) bps = 50;
    return (unsigned long)((long)pWindow * pBlock * 60L / bps + 60);
}

static void numStr(long v, char *out)
{
    char t[12];
    short n = 0, k = 0;
    if (v < 0) v = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out[k++] = t[--n];
    out[k] = 0;
}

static long parseNum(const char **s)
{
    long v = 0;
    while (**s == ' ') (*s)++;
    while (**s >= '0' && **s <= '9') { v = v * 10 + (**s - '0'); (*s)++; }
    return v;
}

static int startsWith(const char *s, const char *pre) { return !strncmp(s, pre, strlen(pre)); }

static int ensureBuf(void)
{
    if (xbuf) return 1;
    xbufSize = 32768;
    while (xbufSize >= 4096 && !(xbuf = NewPtr(xbufSize))) xbufSize /= 2;
    return xbuf != NULL;
}

/* ------------------------------------------------------------------ commands */
static void sendCmd(const char *s, short job)
{
    cmdNo++;
    if (!cmdNo) cmdNo = 1;
    cmdLen = (short)strlen(s);
    if (cmdLen > (short)sizeof cmdText - 1) cmdLen = sizeof cmdText - 1;
    memcpy(cmdText, s, cmdLen);
    link_frame('C', 0, cmdNo, (const unsigned char *)cmdText, cmdLen);
    cmdWaiting = 1;
    cmdTries = 0;
    cmdTick = TickCount();
    pJob = job;
    pState = P_CMD;
    tid = 0;
    ui_stateChanged();
}

int proto_ready(void) { return link_online() && pState == P_IDLE; }
int proto_busy(void) { return pState >= P_CMD && (pJob == J_LIST || pJob == J_GET || pJob == J_PUT); }

static void toIdle(void)
{
    pState = P_IDLE;
    pJob = J_NONE;
    tid = 0;
    cmdWaiting = 0;
    ui_stateChanged();
}

static void freeList(void)
{
    if (listBuf) DisposePtr(listBuf);
    listBuf = NULL;
}

/* the job ended badly: tidy up locally (the partial file is deleted) */
static void dropJob(void)
{
    if (pJob == J_GET) dl_abort();
    if (pJob == J_PUT) ul_end();
    freeList();
}

static void failJob(short code, const char *msg)
{
    short job = pJob;
    dropJob();
    pEnd = TickCount();
    toIdle();
    ui_transferFailed(job, code, msg);
}

/* the job failed here: tell the server to stop it too (the OK CANCEL that follows is not reported again) */
static void abortRemote(short code, const char *msg)
{
    short job = pJob;
    dropJob();
    pEnd = TickCount();
    sendCmd("CANCEL", J_CANCEL);
    cancelledJob = J_NONE;
    ui_transferFailed(job, code, msg);
}

void proto_start(void)
{
    pState = P_GREET;
    pJob = J_NONE;
    greetTick = TickCount();
    cmdWaiting = 0;
    cmdNo = 0;
    serverName[0] = 0;
    ui_stateChanged();
}

static void sendHello(void)
{
    char s[64], n[12];
    strcpy(s, "HELLO 1 ");
    numStr(prefs.window > 0 ? prefs.window : 4, n);
    strcat(s, n);
    strcat(s, " ");
    numStr(prefs.block > 0 ? prefs.block : 1024, n);
    strcat(s, n);
    strcat(s, " ");
    numStr(ser_lineBps(prefs.baud), n);
    strcat(s, n);
    sendCmd(s, J_HELLO);
}

void proto_line(const char *s)
{
    if (pState == P_GREET && startsWith(s, "SHUTTLE ")) sendHello();
}

int proto_list(const char *path)
{
    char s[300];
    if (!proto_ready()) return 0;
    strcpy(s, "LIST ");
    strncat(s, path[0] ? path : ":", 280);
    sendCmd(s, J_LIST);
    return 1;
}

int proto_get(const char *path, long dlen, long rlen)
{
    char s[300];
    if (!proto_ready() || !ensureBuf()) return 0;
    strcpy(s, "GET ");
    strncat(s, path, 280);
    expDlen = dlen;
    expRlen = rlen;
    pDone = 0;
    pTotal = 128 + ((dlen + 127) & ~127L) + ((rlen + 127) & ~127L);
    pStart = TickCount();
    sendCmd(s, J_GET);
    return 1;
}

int proto_put(const unsigned char *name, long total)
{
    char s[300], n[12];
    if (!proto_ready() || !ensureBuf()) return 0;
    strcpy(s, "PUT ");
    memcpy(s + 4, name + 1, name[0]);
    s[4 + name[0]] = 0;
    strcat(s, " ");
    numStr(total, n);
    strcat(s, n);
    sTotal = total;
    pDone = 0;
    pTotal = total;
    pStart = TickCount();
    sendCmd(s, J_PUT);
    return 1;
}

void proto_cancel(void)
{
    if (!proto_busy()) return;
    cancelledJob = pJob;
    dropJob();
    pEnd = TickCount();
    sendCmd("CANCEL", J_CANCEL);       /* frames of the old transfer are ignored from now on (tid 0) */
}

void proto_disconnect(void)
{
    if (!link_online() || pState == P_GREET || pJob == J_BYE) { link_disconnect(); return; }
    if (proto_busy()) {
        short job = pJob;
        dropJob();
        ui_transferFailed(job, E_CANCEL, "Cancelled.");
    }
    sendCmd("BYE", J_BYE);
}

void proto_lost(void)
{
    short job = pJob, busy = proto_busy();
    if (busy) dropJob();
    pState = P_OFF;
    pJob = J_NONE;
    cmdWaiting = 0;
    if (busy) ui_transferFailed(job, E_LOST, "The line dropped during the transfer.");
    ui_stateChanged();
}

/* ------------------------------------------------------------------ receiving (listings and downloads) */
static void startRecv(unsigned char id, long size)
{
    tid = id;
    rSize = size;
    rN = (size + pBlock - 1) / pBlock;
    rNext = 0;
    rGranted = pWindow;
    rSeen = rNakSeen = rLastNakVal = -1;
    rUsed = 0;
    rHolding = rTries = 0;
    rProgressTick = TickCount();
    pState = P_RECV;
    ui_stateChanged();
}

static int flushRx(void)          /* the buffer to its destination; 0 if that failed (the job has ended) */
{
    OSErr e = noErr;
    if (!rUsed) return 1;
    if (pJob == J_LIST) {
        if (listBuf) BlockMove(xbuf, listBuf + listUsed, rUsed);
        listUsed += rUsed;
    } else {
        Cursor **w = GetCursor(watchCursor);
        if (w) SetCursor(*w);
        e = dl_write((unsigned char *)xbuf, rUsed);
        InitCursor();
        stFlushes++;
    }
    rUsed = 0;
    rProgressTick = TickCount();       /* the write's time doesn't count as silence */
    if (e != noErr) {
        failJob(E_DISK, files_errText(e));
        return 0;
    }
    return 1;
}

static void rxNak(int force)
{
    if (rHolding) {
        rHolding = 0;
        if (!flushRx()) return;
    }
    if (force || rLastNakVal != rNext || TickCount() - rLastNakTick > nakWait()) {
        link_frame('N', tid, (unsigned short)rNext, NULL, 0);
        rLastNakVal = rNext;
        rLastNakTick = TickCount();
        rNakSeen = rSeen;
        if (rNext + pWindow > rGranted) rGranted = rNext + pWindow;
        stNaks++;
    }
}

static void rxComplete(void)
{
    if (!flushRx()) return;
    if (pJob == J_GET) {
        OSErr e = dl_finish();
        if (e != noErr) { failJob(E_DISK, files_errText(e)); return; }
        ui_note("Checking the saved file\311");
        {
            Cursor **w = GetCursor(watchCursor);
            if (w) SetCursor(*w);
            e = dl_verify((unsigned char *)xbuf, xbufSize);   /* the line is quiet: the last ack isn't sent yet */
            InitCursor();
        }
        if (e != noErr) {
            dlVerifyFailed = 1;
            dl_discard();
            link_frame('A', tid, (unsigned short)rN, NULL, 0);    /* the server's part went fine */
            pEnd = TickCount();
            toIdle();
            ui_transferFailed(J_GET, E_DISK, files_errText(e));
            return;
        }
    }
    pEnd = TickCount();
    link_frame('A', tid, (unsigned short)rN, NULL, 0);
    pState = P_DONEWAIT;
    doneTick = TickCount();
    doneTries = 0;
}

static void jobComplete(void)
{
    short job = pJob;
    toIdle();
    if (job == J_LIST) {
        ui_listing(listBuf ? (const char *)listBuf : "", listBuf ? listUsed : 0);
        freeList();
    } else if (job == J_GET) ui_transferDone(J_GET, "");
}

static void rxData(unsigned short block, const unsigned char *p, short n)
{
    short d = (short)(block - (unsigned short)rNext);
    long full = rNext + d;
    int rewound = full <= rNakSeen;
    if (full > rSeen) rSeen = full;
    if (d == 0 && rNext < rN) {
        long want = rSize - rNext * pBlock;
        if (want > pBlock) want = pBlock;
        if (n != want) { rxNak(0); return; }
        BlockMove(p, xbuf + rUsed, n);
        rUsed += n;
        rNext++;
        pDone += n;
        stBlocks++;
        rProgressTick = TickCount();
        rTries = 0;
        if (rNext == rN) { rxComplete(); return; }
        if (!rHolding && rUsed + (long)pWindow * pBlock <= xbufSize) {
            link_frame('A', tid, (unsigned short)rNext, NULL, 0);
            if (rNext + pWindow > rGranted) rGranted = rNext + pWindow;
        } else rHolding = 1;
        if (rHolding && rNext >= rGranted) {   /* everything in flight is here: the line is quiet */
            rHolding = 0;
            if (!flushRx()) return;
            link_frame('A', tid, (unsigned short)rNext, NULL, 0);
            rGranted = rNext + pWindow;
        }
    } else if (d < 0) {
        if (!rHolding) link_frame('A', tid, (unsigned short)rNext, NULL, 0);
    } else rxNak(rewound);
}

/* ------------------------------------------------------------------ sending (uploads) */
static void startSend(unsigned char id)
{
    tid = id;
    sN = (sTotal + pBlock - 1) / pBlock;
    sBase = sNxt = sChunkStart = sChunkEnd = 0;
    sTries = 0;
    sProgressTick = TickCount();
    pState = P_SEND;
    ui_stateChanged();
}

static void sendBlock(long k)
{
    long len = sTotal - k * pBlock;
    if (len > pBlock) len = pBlock;
    link_frame('D', tid, (unsigned short)k, (const unsigned char *)xbuf + (k - sChunkStart) * pBlock, (short)len);
}

static void txPoll(void)
{
    if (sBase >= sN) {
        pState = P_SAVEWAIT;
        saveTick = TickCount();
        saveTries = 0;
        return;
    }
    if (sBase == sChunkEnd) {            /* everything read so far is acked: read the next piece */
        long blocks = xbufSize / pBlock, bytes;
        OSErr e;
        sChunkStart = sChunkEnd;
        sChunkEnd = sChunkStart + blocks;
        if (sChunkEnd > sN) sChunkEnd = sN;
        bytes = sTotal - sChunkStart * pBlock;
        if (bytes > blocks * pBlock) bytes = blocks * pBlock;
        e = ul_read((unsigned char *)xbuf, bytes);
        sProgressTick = TickCount();
        if (e != noErr) { abortRemote(E_DISK, files_errText(e)); return; }
    }
    while (sNxt < sBase + pWindow && sNxt < sChunkEnd && link_writeRoom() >= 2L * pBlock + 32) {
        sendBlock(sNxt);
        sNxt++;
    }
    if (sBase < sNxt && TickCount() - sProgressTick > 2 * nakWait() + 180) {
        if (++sTries > 8) { abortRemote(E_LOST, "The MacBook stopped answering."); return; }
        stTimeouts++;
        stResends += sNxt - sBase;
        sNxt = sBase;
        sProgressTick = TickCount();
    }
}

static void txAck(unsigned short v, int nak)
{
    unsigned short dd = (unsigned short)(v - (unsigned short)sBase);
    long full;
    if ((long)dd > sNxt - sBase) return;
    full = sBase + dd;
    if (full > sBase || nak) { sProgressTick = TickCount(); sTries = 0; }
    if (full > sBase) {
        pDone += (full - sBase) * pBlock;
        if (pDone > sTotal) pDone = sTotal;
        sBase = full;
    }
    if (nak && full < sNxt) {
        stResends += sNxt - full;
        sNxt = full;
    }
}

/* ------------------------------------------------------------------ replies */
static void reply(const char *s)
{
    const char *p = s;
    if (startsWith(s, "ERR ")) {
        const char *m = s + 4;
        while (*m && *m != ' ') m++;
        while (*m == ' ') m++;
        if (pJob == J_HELLO) {
            ui_error(E_SERVER, *m ? m : "The server refused the connection.");
            link_disconnect();
            return;
        }
        if (pJob == J_BYE) { toIdle(); link_hangupNow(); return; }
        if (pJob == J_CANCEL) {
            toIdle();
            if (cancelledJob) ui_transferFailed(cancelledJob, E_CANCEL, "Cancelled.");
            return;
        }
        failJob(E_SERVER, *m ? m : "The server reported an error.");
        return;
    }
    switch (pJob) {
    case J_HELLO:
        if (startsWith(s, "OK HELLO ")) {
            p = s + 9;
            parseNum(&p);                                   /* version */
            pWindow = (short)parseNum(&p);
            pBlock = (short)parseNum(&p);
            if (pWindow < 1 || pWindow > 8) pWindow = 4;
            if (pBlock < 64 || pBlock > 1024) pBlock = 1024;
            while (*p == ' ') p++;
            strncpy(serverName, p, sizeof serverName - 1);
            serverName[sizeof serverName - 1] = 0;
            toIdle();
            ui_connected();
        }
        break;
    case J_LIST:
        if (startsWith(s, "OK LIST ")) {
            long id, size;
            p = s + 8;
            id = parseNum(&p);
            size = parseNum(&p);
            freeList();
            listBuf = NewPtr(size + 1);
            listUsed = 0;
            if (!listBuf || !ensureBuf()) { failJob(E_SERVER, "Not enough memory for that folder's list."); return; }
            pDone = 0;
            pTotal = size;
            pStart = TickCount();
            startRecv((unsigned char)id, size);
            if (rN == 0) rxComplete();
        }
        break;
    case J_GET:
        if (startsWith(s, "OK GET ")) {
            long id, size, dl, rl;
            p = s + 7;
            id = parseNum(&p);
            size = parseNum(&p);
            dl = parseNum(&p);
            rl = parseNum(&p);
            if (dl != expDlen || rl != expRlen) {
                failJob(E_FILE, "The file changed on the MacBook since the list was made. Choose Refresh and try again.");
                return;
            }
            pTotal = size;
            pStart = TickCount();
            startRecv((unsigned char)id, size);
        }
        break;
    case J_PUT:
        if (startsWith(s, "OK PUT ")) {
            p = s + 7;
            pStart = TickCount();
            startSend((unsigned char)parseNum(&p));
        }
        break;
    case J_CANCEL:
        toIdle();
        if (cancelledJob) ui_transferFailed(cancelledJob, E_CANCEL, "Cancelled.");
        break;
    case J_BYE:
        pState = P_IDLE;
        pJob = J_NONE;
        link_hangupNow();
        break;
    }
}

static void lateReply(const char *s)  /* a second reply to the command that started the transfer */
{
    if (startsWith(s, "DONE ")) {
        if (pState == P_DONEWAIT) jobComplete();
        return;
    }
    if (pJob == J_PUT && (pState == P_SEND || pState == P_SAVEWAIT)) {
        if (startsWith(s, "OK SAVED ")) {
            const char *p = s + 9;
            static char msg[80];
            parseNum(&p);
            while (*p == ' ') p++;
            strcpy(msg, p);
            pDone = pTotal;
            pEnd = TickCount();
            ul_end();
            toIdle();
            ui_transferDone(J_PUT, msg);
        } else if (startsWith(s, "ERR ")) {
            const char *m = s + 4;
            while (*m && *m != ' ') m++;
            while (*m == ' ') m++;
            failJob(E_SERVER, m);
        }
    }
}

void proto_frame(unsigned char type, unsigned char id, unsigned short block, const unsigned char *p, short n)
{
    if (type == 'R') {
        static char s[260];
        if (n > 255) n = 255;
        memcpy(s, p, n);
        s[n] = 0;
        if (block != cmdNo) return;                       /* an answer to an older command */
        if (startsWith(s, "NOTE ")) {
            cmdTick = rProgressTick = TickCount();
            ui_note(s + 5);
            return;
        }
        if (cmdWaiting) {
            cmdWaiting = 0;
            reply(s);
        } else lateReply(s);
        return;
    }
    if (!id || id != tid) { if (type == 'D') stLateFrames++; return; }
    if (type == 'D') {
        if (pState == P_RECV) rxData(block, p, n);
        else if (pState == P_DONEWAIT) link_frame('A', tid, (unsigned short)rN, NULL, 0);
    } else if ((type == 'A' || type == 'N') && pState == P_SEND) {
        txAck(block, type == 'N');
    }
}

void proto_bad(void)
{
    if (pState == P_RECV) rxNak(0);
}

/* ------------------------------------------------------------------ timers */
void proto_poll(void)
{
    unsigned long now = TickCount();
    if (!link_online()) return;
    if (pState == P_GREET) {
        if (now - greetTick > 300) sendHello();          /* the greeting may have been missed */
        return;
    }
    if (cmdWaiting && now - cmdTick > nakWait() + 180) {
        short limit = (pJob == J_CANCEL || pJob == J_BYE) ? 2 : 4;
        if (++cmdTries > limit) {
            cmdWaiting = 0;
            if (pJob == J_HELLO) {
                ui_error(E_NOSERVER, "Connected, but no Shuttle server answered. Is the dial string right (Connection > Settings\311)?");
                link_disconnect();
            } else if (pJob == J_CANCEL) {
                toIdle();
                if (cancelledJob) ui_transferFailed(cancelledJob, E_CANCEL, "Cancelled.");
            } else if (pJob == J_BYE) {
                toIdle();
                link_disconnect();
            } else failJob(E_LOST, "The MacBook doesn't answer.");
            return;
        }
        link_frame('C', 0, cmdNo, (const unsigned char *)cmdText, cmdLen);
        cmdTick = now;
    }
    switch (pState) {
    case P_RECV:
        if (now - rProgressTick > nakWait() + 120) {
            if (++rTries > 10) { failJob(E_LOST, "The MacBook stopped sending."); return; }
            stTimeouts++;
            rProgressTick = now;
            rxNak(1);
        }
        break;
    case P_DONEWAIT:
        if (now - doneTick > 120) {
            if (++doneTries > 3) { jobComplete(); return; }   /* everything is here anyway */
            link_frame('A', tid, (unsigned short)rN, NULL, 0);
            doneTick = now;
        }
        break;
    case P_SEND:
        txPoll();
        break;
    case P_SAVEWAIT:
        if (now - saveTick > 300) {
            if (++saveTries > 5) { failJob(E_LOST, "The MacBook didn't confirm the file."); return; }
            if (sN > 0) sendBlock(sN - 1);               /* the last block is still in xbuf */
            saveTick = now;
        }
        break;
    default:
        break;
    }
}

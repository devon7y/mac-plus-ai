/*
 * Shuttle: the line to the server (dialling copied from Ask AI's link.c, then frames).
 *
 * 1. Dialling (Hayes command set): "AT" until the modem says OK (if it doesn't, it may still be
 *    online from last time: guard time, "+++", ATH, and try again), then the init string, then
 *    ATDT <dial string> and wait for CONNECT. Echo and numeric result codes are tolerated, and so is
 *    the Plus Modem's "PLUS MODEM READY" banner. A dial string starting with "AT" is sent as it is;
 *    "DIRECT" skips the modem (a plain serial line to the server).
 * 2. Online, the bytes are split into text lines (CR) and frames (0x7E ... CR, escaped, CRC-16): see
 *    ../PROTOCOL.md. Frames go to proto.c; "NO CARRIER" from the modem means the line is gone.
 * 3. Hanging up: proto.c sends BYE and the server hangs up (the modem says NO CARRIER), else guard,
 *    "+++", ATH; then the port is closed.
 * Everything is non-blocking: link_poll() is called from the event loop.
 */
#include <OSUtils.h>
#include <Events.h>
#include <string.h>
#include "serial.h"
#include "link.h"

Prefs prefs;
short linkState;
long linkRx, linkTx, linkBadFrames;

static unsigned long stateTick;
static short tries, escTried, direct;
static char dialShown[64];

/* the byte parser */
static char tline[256];
static short tlen;
static unsigned char fbuf[1040];
static short flen, fmode;           /* 0 text, 1 in a frame, 2 in a frame after ESC */

/* ------------------------------------------------------------------ CRC-16/XMODEM */
static unsigned short crcTab[256];

void link_init(void)
{
    short i, k;
    for (i = 0; i < 256; i++) {
        unsigned short c = (unsigned short)(i << 8);
        for (k = 0; k < 8; k++) c = (unsigned short)((c & 0x8000) ? (c << 1) ^ 0x1021 : (c << 1));
        crcTab[i] = c;
    }
}

unsigned short crc16(const unsigned char *p, long n, unsigned short crc)
{
    while (n--) crc = (unsigned short)((crc << 8) ^ crcTab[((crc >> 8) ^ *p++) & 0xFF]);
    return crc;
}

/* ------------------------------------------------------------------ sending */
static void enter(short s)
{
    linkState = s;
    stateTick = TickCount();
    ui_stateChanged();
}

static unsigned long since(void) { return TickCount() - stateTick; }

static void sendStr(const char *s)
{
    long n = (long)strlen(s);
    ser_write(s, n);
    linkTx += n;
}

long link_writeRoom(void) { return ser_writeRoom(); }

static int esc(unsigned char c) { return c == 0x0D || c == 0x11 || c == 0x13 || c == 0x7D || c == 0x7E || c == 0xFF; }

void link_frame(unsigned char type, unsigned char id, unsigned short block, const unsigned char *p, short n)
{
    static unsigned char out[2 * 1030 + 4];
    unsigned char head[4];
    unsigned short c;
    short k = 0, i;
    head[0] = type;
    head[1] = id;
    head[2] = (unsigned char)(block >> 8);
    head[3] = (unsigned char)block;
    c = crc16(head, 4, 0);
    c = crc16(p, n, c);
    out[k++] = 0x7E;
    for (i = 0; i < 4; i++) {
        if (esc(head[i])) { out[k++] = 0x7D; out[k++] = (unsigned char)(head[i] ^ 0x20); }
        else out[k++] = head[i];
    }
    for (i = 0; i < n; i++) {
        unsigned char b = p[i];
        if (esc(b)) { out[k++] = 0x7D; out[k++] = (unsigned char)(b ^ 0x20); }
        else out[k++] = b;
    }
    head[0] = (unsigned char)(c >> 8);
    head[1] = (unsigned char)c;
    for (i = 0; i < 2; i++) {
        if (esc(head[i])) { out[k++] = 0x7D; out[k++] = (unsigned char)(head[i] ^ 0x20); }
        else out[k++] = head[i];
    }
    out[k++] = 0x0D;
    ser_write((const char *)out, k);
    linkTx += k;
}

static void p2c(const unsigned char *p, char *c, short max)
{
    short n = p[0] < max - 1 ? p[0] : (short)(max - 1);
    memcpy(c, p + 1, n);
    c[n] = 0;
}

static int startsWith(const char *s, const char *pre) { return !strncmp(s, pre, strlen(pre)); }

static int eqi(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* ------------------------------------------------------------------ hanging up */
static void closeAll(void)
{
    short was = linkState;
    ser_close();
    enter(L_OFF);
    if (was >= L_ONLINE && was <= L_HESC3) proto_lost();
}

static void fail(short code, const char *msg)
{
    ui_error(code, msg);
    closeAll();
}

void link_disconnect(void)
{
    switch (linkState) {
    case L_OFF:
        return;
    case L_ONLINE:
        if (direct) { closeAll(); return; }
        enter(L_HESC1);                 /* proto.c sends BYE first when it can (link_hangupNow) */
        return;
    case L_BYE: case L_HESC1: case L_HESC2: case L_HESC3:
        closeAll();                     /* asked twice: just drop the line */
        return;
    default:                            /* still talking to the modem, or the terminal */
        if (linkState == L_DIAL) sendStr("\r");   /* any character aborts a dial */
        closeAll();
        return;
    }
}

void link_hangupNow(void)
{
    if (linkState != L_ONLINE) return;
    if (direct) closeAll();
    else enter(L_BYE);                  /* the server closes; the modem then says NO CARRIER */
}

void link_terminal(void)
{
    OSErr e;
    if (linkState != L_OFF) return;
    e = ser_open(prefs.port, prefs.baud, prefs.flow);
    if (e != noErr) {
        ui_error(E_PORT, prefs.port ? "Shuttle couldn't open the printer port. If AppleTalk is on, turn it off in the Chooser."
                                    : "Shuttle couldn't open the modem port. Is another program using it?");
        return;
    }
    linkRx = linkTx = 0;
    enter(L_TERM);
}

void link_termSend(const char *s, short n)
{
    if (linkState != L_TERM) return;
    ser_write(s, n);
    if (!(n == 3 && !memcmp(s, "+++", 3))) ser_write("\r", 1);   /* "+++" alone: the escape, no Return */
}

int link_online(void) { return linkState == L_ONLINE; }

/* ------------------------------------------------------------------ connecting */
void link_connect(void)
{
    OSErr e;
    char d[64];
    if (linkState != L_OFF) return;
    p2c(prefs.dial, d, sizeof d);
    if (!d[0]) {
        ui_error(E_DIAL, "Enter what to dial first: Connection > Settings\311 (normally \"mac\").");
        return;
    }
    e = ser_open(prefs.port, prefs.baud, prefs.flow);
    if (e != noErr) {
        ui_error(E_PORT, prefs.port ? "Shuttle couldn't open the printer port. If AppleTalk is on, turn it off in the Chooser."
                                    : "Shuttle couldn't open the modem port. Is another program using it?");
        return;
    }
    strcpy(dialShown, d);
    direct = eqi(d, "DIRECT");
    tries = escTried = 0;
    tlen = 0;
    fmode = 0;
    linkRx = linkTx = 0;
    if (direct) {
        enter(L_ONLINE);
        proto_start();
        return;
    }
    sendStr("\rAT\r");                  /* the CR first ends any half-typed command */
    enter(L_WAKE);
}

static void dialNow(void)
{
    char d[64];
    p2c(prefs.dial, d, sizeof d);
    if (startsWith(d, "AT") || startsWith(d, "at")) sendStr(d);
    else { sendStr("ATDT"); sendStr(d); }
    sendStr("\r");
    enter(L_DIAL);
}

static void sendInit(void)
{
    char s[66];
    p2c(prefs.init, s, sizeof s);
    if (s[0]) {
        sendStr(s);
        sendStr("\r");
        enter(L_INIT);
    } else dialNow();
}

/* ------------------------------------------------------------------ result codes from the modem */
enum { R_NONE, R_OK, R_CONNECT, R_NOCARRIER, R_ERROR, R_NODIALTONE, R_BUSY, R_NOANSWER, R_ECHO, R_OTHER };

static int result(const char *s)
{
    if (startsWith(s, "AT") || startsWith(s, "at") || !strcmp(s, "+++")) return R_ECHO;
    if (!strcmp(s, "OK") || !strcmp(s, "0")) return R_OK;
    if (startsWith(s, "CONNECT")) return R_CONNECT;
    if (!strcmp(s, "NO CARRIER") || !strcmp(s, "3")) return R_NOCARRIER;
    if (!strcmp(s, "ERROR") || !strcmp(s, "4")) return R_ERROR;
    if (!strcmp(s, "NO DIALTONE") || !strcmp(s, "NO DIAL TONE") || !strcmp(s, "6")) return R_NODIALTONE;
    if (!strcmp(s, "BUSY") || !strcmp(s, "7")) return R_BUSY;
    if (!strcmp(s, "NO ANSWER") || !strcmp(s, "8")) return R_NOANSWER;
    if (s[0] >= '0' && s[0] <= '9') {
        const char *p = s;
        while (*p >= '0' && *p <= '9') p++;
        if (!*p) return R_CONNECT;
    }
    return R_OTHER;
}

static void dialFailed(int r)
{
    static char msg[260];
    if (r == R_NOCARRIER || r == R_NOANSWER || r == R_BUSY) {
        strcpy(msg, "The modem couldn't reach \"");
        strcat(msg, dialShown);
        strcat(msg, r == R_BUSY ? "\" (BUSY)." : r == R_NOANSWER ? "\" (NO ANSWER)." : "\" (NO CARRIER).");
        strcat(msg, " Is the Shuttle server running on the MacBook, on the same Wi-Fi? Does the modem know where "
                    "it is (the server prints the AT+ALIAS line to type in Modem Terminal)?");
    } else if (r == R_NODIALTONE) {
        strcpy(msg, "The modem isn't on Wi-Fi (NO DIALTONE). Set up Wi-Fi first (see the Read Me).");
    } else if (r == R_ERROR) {
        strcpy(msg, "The modem rejected the dial command (ERROR). If you dial \"mac\", tell the modem the "
                    "MacBook's address with the AT+ALIAS line the server prints.");
    } else {
        strcpy(msg, "No answer from the modem after dialling \"");
        strcat(msg, dialShown);
        strcat(msg, "\".");
    }
    fail(E_DIAL, msg);
}

static void textLine(const char *s)
{
    int r = result(s);
    if (linkState == L_ONLINE || linkState == L_BYE) {
        if (r == R_NOCARRIER) {                     /* the TCP connection is gone */
            if (linkState == L_BYE) closeAll();
            else fail(E_LOST, "The connection to the MacBook was lost (the modem said NO CARRIER).");
            return;
        }
        if (linkState == L_ONLINE) proto_line(s);
        return;
    }
    if (r == R_ECHO) return;
    switch (linkState) {
    case L_WAKE:
        if (r == R_OK) sendInit();
        else if (startsWith(s, "SHUTTLE ")) { enter(L_ONLINE); proto_start(); }   /* still online from before */
        break;
    case L_ESC2:
        if (r == R_OK) { sendStr("ATH\r"); enter(L_ESC3); }
        break;
    case L_ESC3:
        if (r == R_OK) { sendStr("AT\r"); tries = 0; enter(L_WAKE); }
        break;
    case L_INIT:
        if (r == R_OK) dialNow();
        else if (r == R_ERROR) fail(E_INIT, "The modem rejected the init string (ERROR). Check it in Connection > Settings\311");
        break;
    case L_DIAL:
        if (r == R_CONNECT) { enter(L_ONLINE); proto_start(); }
        else if (r != R_OTHER && r != R_OK) dialFailed(r);
        break;
    case L_HESC2:
        if (r == R_OK) { sendStr("ATH\r"); enter(L_HESC3); }
        break;
    case L_HESC3:
        if (r == R_OK || r == R_NOCARRIER) closeAll();
        break;
    default:
        break;
    }
}

static void frameDone(void)
{
    unsigned short c;
    if (flen < 6) { linkBadFrames++; proto_bad(); return; }
    c = (unsigned short)(fbuf[flen - 2] << 8 | fbuf[flen - 1]);
    if (crc16(fbuf, flen - 2, 0) != c) { linkBadFrames++; proto_bad(); return; }
    proto_frame(fbuf[0], fbuf[1], (unsigned short)(fbuf[2] << 8 | fbuf[3]), fbuf + 4, (short)(flen - 6));
}

/* ------------------------------------------------------------------ timeouts */
static void timeouts(void)
{
    unsigned long t = since();
    switch (linkState) {
    case L_WAKE:
        if (t > 90) {
            if (++tries < 3) { sendStr("AT\r"); stateTick = TickCount(); }
            else if (!escTried) { escTried = 1; enter(L_ESC1); }      /* maybe still online */
            else fail(E_NOMODEM, "The modem doesn't answer. Check that it is on, the cable, and that its speed "
                                 "matches Connection > Settings\311 (19200).");
        }
        break;
    case L_ESC1: case L_HESC1:
        if (t > 72 && ser_writeIdle()) {        /* 1.2 s of silence, then the escape */
            sendStr("+++");
            enter(linkState == L_ESC1 ? L_ESC2 : L_HESC2);
        }
        break;
    case L_ESC2: case L_ESC3:
        if (t > 180) { sendStr("AT\r"); tries = 0; enter(L_WAKE); }
        break;
    case L_INIT:
        if (t > 240) fail(E_INIT, "The modem didn't answer the init string. Check it in Connection > Settings\311");
        break;
    case L_DIAL:
        if (t > 60L * 50) dialFailed(R_NONE);
        break;
    case L_BYE:
        if (t > 240) enter(L_HESC1);
        break;
    case L_HESC2: case L_HESC3:
        if (t > 180) closeAll();
        break;
    default:
        break;
    }
}

void link_poll(void)
{
    static unsigned char buf[512];
    long n, i;
    if (linkState == L_OFF) return;
    if (linkState == L_TERM) {
        if ((n = ser_read((char *)buf, sizeof buf)) > 0) {
            linkRx += n;
            ui_termText((const char *)buf, (short)n);
        }
        return;
    }
    /* one chunk per call, then back to the event loop: during a download bytes arrive all the time, and a
       loop until none are waiting would keep the window from being drawn and the Cancel button from
       working (the Serial Driver's 16 KB buffer holds what arrives meanwhile) */
    if ((n = ser_read((char *)buf, sizeof buf)) > 0) {
        linkRx += n;
        for (i = 0; i < n; i++) {
            unsigned char c = buf[i];
            if (fmode == 0) {
                if (c == 0x7E) { fmode = 1; flen = 0; tlen = 0; }
                else if (c == 0x0D) {
                    tline[tlen] = 0;
                    if (tlen) textLine(tline);
                    tlen = 0;
                    if (linkState == L_OFF) return;
                } else if (c != 0x0A && c != 0) {
                    if (tlen < (short)sizeof tline - 1) tline[tlen++] = (char)c;
                }
            } else {
                if (c == 0x7E) { flen = 0; fmode = 1; continue; }
                if (c == 0x0D) {
                    fmode = 0;
                    frameDone();
                    if (linkState == L_OFF) return;
                    continue;
                }
                if (fmode == 2) { fmode = 1; c ^= 0x20; }
                else if (c == 0x7D) { fmode = 2; continue; }
                if (flen < (short)sizeof fbuf) fbuf[flen++] = c;
                else { fmode = 0; linkBadFrames++; proto_bad(); }
            }
        }
    }
    timeouts();
}

const char *link_stateName(void)
{
    switch (linkState) {
    case L_OFF: return "Not connected";
    case L_WAKE: case L_ESC1: case L_ESC2: case L_ESC3: return "Waking the modem\311";
    case L_INIT: return "Setting up the modem\311";
    case L_DIAL: return "Dialling\311";
    case L_ONLINE: return "Connected";
    case L_TERM: return "Modem Terminal";
    default: return "Hanging up\311";
    }
}

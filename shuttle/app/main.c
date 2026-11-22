/*
 * Shuttle: move files between a Macintosh Plus or SE (System 6 or 7) and a MacBook, through a Hayes
 * Wi-Fi modem on the modem port and the Shuttle server (../server/shuttle_server.py).
 *
 * One window, sized to the screen (qd.screenBits.bounds, so Narrow Screen is respected):
 *   Connect/Disconnect button and the status line (state, server, bytes/s)
 *   the folder on the MacBook, with an Up button
 *   the list of files there (name, size, kind; double-click a folder to open it, a file to download)
 *   Download and Send File... buttons, the startup disk's free space
 *   the progress line and bar, with time left, and Cancel
 * plus a Modem Terminal window (lines typed there go straight to the modem, as in Ask AI).
 * Settings live in "Shuttle Prefs" in the System Folder (layout: link.h; build.py writes it too).
 *
 * A debug block (magic 'SHT!') lets MAME Lua scripts follow the app (tests/transfer.lua).
 */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Multiverse.h>
#include <Events.h>
#include <OSUtils.h>
#include <MacMemory.h>
#include <Files.h>
#include <ToolUtils.h>
#include <LowMem.h>
#include <StandardFile.h>
#include <SegLoad.h>
#include <string.h>
#include "serial.h"
#include "link.h"
#include "proto.h"
#include "files.h"

#define MAGIC 0x53485421UL     /* 'SHT!' */

struct Dbg {
    unsigned long magic, version, notMagic;   /*   0,   4,   8 */
    long linkState;      /*  12 */
    long pState;         /*  16 */
    long pJob;           /*  20 */
    long rx, tx;         /*  24,  28: bytes on the line since connecting */
    long items;          /*  32: entries in the list */
    long sel;            /*  36: selected entry (-1 none) */
    long done, total;    /*  40,  44: bytes of the current transfer */
    long bps;            /*  48: bytes per second of the last finished transfer */
    long serErr;         /*  52: serial overrun / framing errors */
    long naks;           /*  56 */
    long timeouts;       /*  60 */
    long flushes;        /*  64: buffer writes to disk */
    long downloads;      /*  68: finished */
    long uploads;        /*  72: finished */
    long cancels;        /*  76 */
    long errors;         /*  80: errors shown */
    long lastErr;        /*  84: E_* */
    long freeMem;        /*  88 */
    long ticks;          /*  92: duration of the last finished transfer */
    long badFrames;      /*  96 */
    long resends;        /* 100: blocks sent again (uploads) */
    long blocks;         /* 104: data blocks accepted */
    long listings;       /* 108: listings received */
    char status[64];     /* 112 */
    char path[64];       /* 176 */
    char msg[64];        /* 240: the last progress or error message */
    long ser[4];         /* 304: serial.c's serDbg */
    short winTop, winLeft, winH, winW;   /* 320: the window's content, global coordinates */
    unsigned short dlCrc[2];             /* 328: CRC-16 of the data and resource fork bytes written */
    long redraws;                        /* 332: full window redraws (update events) */
};
extern unsigned short dlCrc[2];
extern long serDbg[4];
static struct Dbg dbg;

static WindowPtr win, termWin;
static MenuHandle appleM, fileM, editM, connM;
static ControlHandle bConnect, bUp, bDownload, bSend, bCancel, sbar;

/* ------------------------------------------------------------------ layout */
#define M 8
#define ROW_H 12
static Rect statusR, pathR, listBox, headR, rowsR, sbR, freeR, msgR, barR;
static short W, H, visRows;
static short colSize, colKind;          /* right edge of the size column, left edge of the kind column */

/* ------------------------------------------------------------------ the list */
typedef struct {
    char kind;                          /* 'D' folder, 'A' archive, 'F' file */
    unsigned char name[32];             /* Str31 */
    long dlen, rlen, count;
    OSType type, creator;
} Item;
#define MAXITEMS 400
static Item *items;
static short nItems, sel = -1, topRow;
static char path[256];                  /* on the MacBook: "" (the top) or "Games:Arcade" */
static char wantPath[256];              /* the path of the listing on its way */
static char typed[32];
static short typedLen;
static unsigned long typedTick, lastClickTick;
static short lastClickRow = -1;

/* ------------------------------------------------------------------ transfers */
static char msg[160];                   /* the progress line */
static unsigned char xferName[64];
static short xferJob, firstSave = 1, prefsDirty, prefsSaveFailed, startVol;
static unsigned long lastDraw, noteTick;
static char note[80];
static long freeShown = -2;

static const unsigned short icFolder[10] = {0x3800, 0x47C0, 0x4040, 0x7FC0, 0x4040, 0x4040, 0x4040, 0x7FC0, 0, 0};
static const unsigned short icDoc[10] = {0x7E00, 0x4300, 0x4280, 0x43C0, 0x4040, 0x4040, 0x4040, 0x4040, 0x7FC0, 0};
static const unsigned short icApp[10] = {0x0400, 0x0A00, 0x1100, 0x2080, 0x4040, 0x2080, 0x1100, 0x0A00, 0x0400, 0};
static const unsigned short icArc[10] = {0x7FC0, 0x4440, 0x7FC0, 0x4440, 0x4440, 0x4440, 0x4440, 0x7FC0, 0, 0};
static const unsigned short icDisk[10] = {0x7FC0, 0x5140, 0x5F40, 0x4040, 0x5F40, 0x5140, 0x5140, 0x7FC0, 0, 0};

static void setDbgStr(char *dst, const char *s)
{
    short n = (short)strlen(s);
    if (n > 63) n = 63;
    memcpy(dst, s, n);
    dst[n] = 0;
}

static void numStr(long v, char *out, int commas)
{
    char t[12];
    short n = 0, k = 0;
    if (v < 0) v = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) {
        out[k++] = t[--n];
        if (commas && n && n % 3 == 0) out[k++] = ',';
    }
    out[k] = 0;
}

static void sizeStr(long bytes, char *out)
{
    long k = (bytes + 1023) / 1024;
    if (k < 10000) {
        numStr(k, out, 1);
        strcat(out, "K");
    } else {
        char t[12];
        long tenths = (bytes * 10 + 524288) / 1048576;
        numStr(tenths / 10, out, 1);
        strcat(out, ".");
        numStr(tenths % 10, t, 0);
        strcat(out, t);
        strcat(out, "M");
    }
}

static void timeStr(long secs, char *out)
{
    char t[12];
    numStr(secs / 60, out, 0);
    strcat(out, ":");
    if (secs % 60 < 10) strcat(out, "0");
    numStr(secs % 60, t, 0);
    strcat(out, t);
}

static void pset(unsigned char *p, const char *s, short max)
{
    short n = (short)strlen(s);
    if (n > max) n = max;
    p[0] = (unsigned char)n;
    memcpy(p + 1, s, n);
}

static void p2c(const unsigned char *p, char *c, short max)
{
    short n = p[0] < max - 1 ? p[0] : (short)(max - 1);
    memcpy(c, p + 1, n);
    c[n] = 0;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* ------------------------------------------------------------------ drawing helpers */
static void textAt(const char *s, short x, short y)
{
    MoveTo(x, y);
    DrawText(s, 0, (short)strlen(s));
}

static void eraseText(const Rect *r, const char *s, short font, short size)
{
    GrafPtr old;
    GetPort(&old);
    SetPort(win);
    EraseRect(r);
    TextFont(font);
    TextSize(size);
    {
        short n = (short)strlen(s), w = TextWidth(s, 0, n), room = (short)(r->right - r->left);
        const char *t = s;
        while (w > room && n > 1) { t++; n--; w = TextWidth(t, 0, n); }   /* too long: drop from the left */
        MoveTo(r->left, (short)(r->bottom - 4));
        DrawText(t, 0, n);
    }
    TextFont(0);
    TextSize(12);
    SetPort(old);
}

static void drawIcon(const unsigned short *ic, short x, short y)
{
    BitMap bm;
    Rect d;
    bm.baseAddr = (Ptr)ic;
    bm.rowBytes = 2;
    SetRect(&bm.bounds, 0, 0, 16, 10);
    SetRect(&d, x, y, (short)(x + 16), (short)(y + 10));
    CopyBits(&bm, &win->portBits, &bm.bounds, &d, srcOr, NULL);
}

static const char *kindOf(const Item *it)
{
    static char k[24];
    if (it->kind == 'D') return "folder";
    if (it->kind == 'A') return "archive";
    switch (it->type) {
    case 'APPL': return "application";
    case 'TEXT': return "text";
    case 'PNTG': return "MacPaint";
    case 'PICT': return "picture";
    case 'dImg': return "disk image";
    case 'SIT!': case 'SITD': return "StuffIt";
    case 'INIT': return "extension";
    case 'cdev': return "control panel";
    case 'FFIL': return "font";
    case 'DFIL': return "desk accessory";
    case 'ZSYS': return "system file";
    case '????': return "file";
    case 'pref': return "preferences";
    default: break;
    }
    k[0] = '\'';
    memcpy(k + 1, &it->type, 4);
    k[5] = '\'';
    strcpy(k + 6, " file");
    return k;
}

static const unsigned short *iconOf(const Item *it)
{
    if (it->kind == 'D') return icFolder;
    if (it->kind == 'A') return icArc;
    if (it->type == 'APPL') return icApp;
    if (it->type == 'dImg') return icDisk;
    return icDoc;
}

/* ------------------------------------------------------------------ the list */
static void drawRow(short i)
{
    Rect r;
    short y;
    char s[24];
    Item *it;
    if (i < topRow || i >= topRow + visRows) return;
    SetRect(&r, rowsR.left, (short)(rowsR.top + (i - topRow) * ROW_H), rowsR.right,
            (short)(rowsR.top + (i - topRow + 1) * ROW_H));
    EraseRect(&r);
    if (i >= nItems) return;
    it = &items[i];
    TextFont(3);
    TextSize(9);
    y = (short)(r.top + 9);
    drawIcon(iconOf(it), (short)(r.left + 3), (short)(r.top + 1));
    MoveTo((short)(r.left + 20), y);
    {
        short n = it->name[0], room = (short)(colSize - 60 - r.left - 20);
        while (n > 1 && TextWidth(it->name + 1, 0, n) > room) n--;
        DrawText(it->name + 1, 0, n);
        if (n < it->name[0]) DrawChar('\311');
    }
    if (it->kind == 'F' || it->kind == 'A') sizeStr(it->kind == 'F' ? it->dlen + it->rlen : it->dlen, s);
    else { numStr(it->count, s, 0); strcat(s, it->count == 1 ? " item" : " items"); }
    MoveTo((short)(colSize - TextWidth(s, 0, (short)strlen(s))), y);
    DrawText(s, 0, (short)strlen(s));
    textAt(kindOf(it), colKind, y);
    TextFont(0);
    TextSize(12);
    if (i == sel) InvertRect(&r);
}

static void drawRows(void)
{
    short i;
    for (i = topRow; i < topRow + visRows; i++) drawRow(i);
}

static void drawListHead(void)
{
    TextFont(3);
    TextSize(9);
    EraseRect(&headR);
    textAt("Name", (short)(headR.left + 20), (short)(headR.top + 10));
    textAt("Size", (short)(colSize - TextWidth("Size", 0, 4)), (short)(headR.top + 10));
    textAt("Kind", colKind, (short)(headR.top + 10));
    MoveTo(listBox.left, (short)(headR.bottom - 1));
    LineTo((short)(sbR.left - 1), (short)(headR.bottom - 1));
    TextFont(0);
    TextSize(12);
}

static short maxTop(void) { return (short)(nItems > visRows ? nItems - visRows : 0); }

static void setScroll(void)
{
    SetControlMaximum(sbar, maxTop());
    SetControlValue(sbar, topRow);
    HiliteControl(sbar, maxTop() > 0 ? 0 : 255);
}

static void scrollTo(short t)
{
    if (t > maxTop()) t = maxTop();
    if (t < 0) t = 0;
    if (t == topRow) return;
    topRow = t;
    SetPort(win);
    drawRows();
    setScroll();
}

static void updateButtons(void);

static void select(short i)
{
    short old = sel;
    if (i >= nItems) i = (short)(nItems - 1);
    if (i < 0 && nItems) i = 0;
    SetPort(win);
    sel = i;
    if (old != sel) { drawRow(old); drawRow(sel); }
    if (sel >= 0 && sel < topRow) scrollTo(sel);
    else if (sel >= topRow + visRows) scrollTo((short)(sel - visRows + 1));
    dbg.sel = sel;
    updateButtons();
}

static unsigned long hex32(const char **p)
{
    unsigned long v = 0;
    short k;
    while (**p == ' ') (*p)++;
    for (k = 0; k < 8 && **p; k++, (*p)++) {
        char c = **p;
        v = (v << 4) | (unsigned long)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
    }
    return v;
}

static long num(const char **p)
{
    long v = 0;
    while (**p == ' ') (*p)++;
    while (**p >= '0' && **p <= '9') { v = v * 10 + (**p - '0'); (*p)++; }
    return v;
}

void ui_listing(const char *text, long n)
{
    long i = 0;
    nItems = 0;
    while (i < n && nItems < MAXITEMS) {
        char line[300];
        short k = 0;
        const char *p;
        Item *it = &items[nItems];
        while (i < n && text[i] != '\r') { if (k < 299) line[k++] = text[i]; i++; }
        i++;
        line[k] = 0;
        if (k < 3 || line[1] != ' ') continue;
        p = line + 2;
        it->kind = line[0];
        it->dlen = it->rlen = it->count = 0;
        it->type = it->creator = '????';
        if (line[0] == 'D') it->count = num(&p);
        else if (line[0] == 'A') it->dlen = num(&p);
        else if (line[0] == 'F') {
            it->dlen = num(&p);
            it->rlen = num(&p);
            it->type = (OSType)hex32(&p);
            it->creator = (OSType)hex32(&p);
        } else continue;
        if (*p == ' ') p++;
        pset(it->name, p, 31);
        if (it->name[0]) nItems++;
    }
    strcpy(path, wantPath);
    topRow = 0;
    sel = nItems ? 0 : -1;
    dbg.items = nItems;
    dbg.sel = sel;
    dbg.listings++;
    setDbgStr(dbg.path, path);
    SetPort(win);
    InvalRect(&win->portRect);
    setScroll();
    updateButtons();
}

static void requestList(const char *p)
{
    strncpy(wantPath, p, sizeof wantPath - 1);
    wantPath[sizeof wantPath - 1] = 0;
    if (proto_list(wantPath)) {
        strcpy(msg, "Opening ");
        strcat(msg, wantPath[0] ? wantPath : "the Shuttle folder");
        strcat(msg, "\311");
        SetPort(win);
        InvalRect(&msgR);
    }
}

static void openItem(void)
{
    char p[256];
    Item *it;
    if (sel < 0 || sel >= nItems || !proto_ready()) return;
    it = &items[sel];
    if (it->kind == 'F') return;
    strcpy(p, path);
    if (p[0]) strcat(p, ":");
    if (strlen(p) + it->name[0] < sizeof p - 1) {
        short k = (short)strlen(p);
        memcpy(p + k, it->name + 1, it->name[0]);
        p[k + it->name[0]] = 0;
        requestList(p);
    }
}

static void goUp(void)
{
    char p[256], *c;
    if (!path[0] || !proto_ready()) return;
    strcpy(p, path);
    c = strrchr(p, ':');
    if (c) *c = 0;
    else p[0] = 0;
    requestList(p);
}

/* ------------------------------------------------------------------ status, path, progress */
static void statusText(char *s)
{
    if (note[0] && TickCount() - noteTick < 240) { strcpy(s, note); return; }
    note[0] = 0;
    if (linkState == L_ONLINE) {
        if (pState == P_GREET || (pState == P_CMD && pJob == J_HELLO)) strcpy(s, "Connected, saying hello\311");
        else if (pJob == J_BYE) strcpy(s, "Hanging up\311");
        else {
            strcpy(s, "Connected to ");
            strcat(s, serverName[0] ? serverName : "the MacBook");
        }
        if (pState == P_RECV || pState == P_SEND) {
            long t = (long)(TickCount() - pStart);
            if (t > 60 && pDone > 0) {
                char n[16];
                strcat(s, "  \245  ");
                numStr(pDone * 60 / t, n, 1);
                strcat(s, n);
                strcat(s, " bytes/s");
            }
        }
    } else if (linkState == L_DIAL) {
        char d[64];
        p2c(prefs.dial, d, sizeof d);
        strcpy(s, "Dialling ");
        strcat(s, d);
        strcat(s, "\311");
    } else strcpy(s, link_stateName());
}

static void drawStatus(void)
{
    char s[120];
    statusText(s);
    setDbgStr(dbg.status, s);
    eraseText(&statusR, s, 0, 12);
}

static void drawPath(void)
{
    char s[300];
    strcpy(s, linkState == L_ONLINE ? "Folder: Shuttle" : "Folder: (not connected)");
    if (linkState == L_ONLINE && path[0]) { strcat(s, ":"); strcat(s, path); }
    eraseText(&pathR, s, 0, 12);
}

static void drawFree(void)
{
    char s[80], n[16];
    long alb, f = vol_free(startVol, &alb);
    Str255 vn;
    HParamBlockRec pb;
    freeShown = f;
    memset(&pb, 0, sizeof pb);
    vn[0] = 0;
    pb.volumeParam.ioNamePtr = vn;
    pb.volumeParam.ioVRefNum = startVol;
    PBHGetVInfoSync(&pb);
    p2c(vn, s, 32);
    strcat(s, ": ");
    sizeStr(f, n);
    strcat(s, n);
    strcat(s, " free");
    SetPort(win);
    EraseRect(&freeR);
    TextFont(3);
    TextSize(9);
    MoveTo((short)(freeR.right - TextWidth(s, 0, (short)strlen(s))), (short)(freeR.bottom - 4));
    DrawText(s, 0, (short)strlen(s));
    TextFont(0);
    TextSize(12);
}

static void progressText(char *s)
{
    if (proto_busy() && (pState == P_RECV || pState == P_SEND || pState == P_SAVEWAIT || pState == P_DONEWAIT)
        && xferJob != J_LIST) {
        char n[16];
        long t = (long)(TickCount() - pStart), rate = t > 60 && pDone > 0 ? pDone * 60 / t : 0;
        strcpy(s, xferJob == J_GET ? "Receiving " : "Sending ");
        p2c(xferName, s + strlen(s), 40);
        strcat(s, ": ");
        sizeStr(pDone, n);
        strcat(s, n);
        strcat(s, " of ");
        sizeStr(pTotal, n);
        strcat(s, n);
        if (rate > 0) {
            strcat(s, "  \245  ");
            timeStr((pTotal - pDone) / rate + 1, n);
            strcat(s, n);
            strcat(s, " left");
        }
        return;
    }
    strcpy(s, msg);
}

static void drawProgress(void)
{
    char s[200];
    Rect r = barR;
    progressText(s);
    setDbgStr(dbg.msg, s);
    eraseText(&msgR, s, 3, 9);
    SetPort(win);
    if (proto_busy() && xferJob != J_LIST && pTotal > 0) {
        FrameRect(&r);
        InsetRect(&r, 1, 1);
        EraseRect(&r);
        r.right = (short)(r.left + (long)(r.right - r.left) * (pDone > pTotal ? pTotal : pDone) / pTotal);
        FillRect(&r, &qd.gray);
    } else EraseRect(&r);
    lastDraw = TickCount();
}

static void updateButtons(void)
{
    short ready = proto_ready(), busy = proto_busy();
    Str255 t;
    GetControlTitle(bConnect, t);
    if (linkState == L_OFF || linkState == L_TERM) {
        if (t[1] != 'C') SetControlTitle(bConnect, "\pConnect");
    } else if (t[1] != 'D') SetControlTitle(bConnect, "\pDisconnect");
    HiliteControl(bConnect, linkState == L_TERM ? 255 : 0);
    HiliteControl(bUp, ready && path[0] ? 0 : 255);
    HiliteControl(bDownload, ready && sel >= 0 && sel < nItems ? 0 : 255);
    HiliteControl(bSend, ready ? 0 : 255);
    HiliteControl(bCancel, busy && pJob != J_CANCEL ? 0 : 255);
    if (sel >= 0 && sel < nItems && items[sel].kind != 'F') {
        GetControlTitle(bDownload, t);
        if (t[1] != 'O') SetControlTitle(bDownload, "\pOpen");
    } else {
        GetControlTitle(bDownload, t);
        if (t[1] != 'D') SetControlTitle(bDownload, "\pDownload");
    }
}

static void updateMenus(void)
{
    short ready = proto_ready();
    SetMenuItemText(connM, 1, linkState == L_OFF || linkState == L_TERM ? "\pConnect" : "\pDisconnect");
    if (linkState == L_TERM) DisableItem(connM, 1); else EnableItem(connM, 1);
    if (ready) { EnableItem(fileM, 1); EnableItem(fileM, 2); EnableItem(connM, 2); }
    else { DisableItem(fileM, 1); DisableItem(fileM, 2); DisableItem(connM, 2); }
    if (proto_busy()) EnableItem(fileM, 3); else DisableItem(fileM, 3);
    if (linkState == L_OFF || linkState == L_TERM) EnableItem(connM, 5); else DisableItem(connM, 5);
}

static void drawAll(void)
{
    dbg.redraws++;
    SetPort(win);
    EraseRect(&win->portRect);
    DrawControls(win);
    drawStatus();
    drawPath();
    FrameRect(&listBox);
    drawListHead();
    drawRows();
    drawFree();
    drawProgress();
}

static void layout(void)
{
    Rect scr = qd.screenBits.bounds, r;
    short sw = (short)(scr.right - scr.left), sh = (short)(scr.bottom - scr.top), mb = LMGetMBarHeight();
    W = (short)(sw - 16 > 480 ? 480 : sw - 16);
    H = (short)(sh - mb - 19 - 8 > 300 ? 300 : sh - mb - 19 - 8);
    SetRect(&r, (short)(scr.left + (sw - W) / 2), (short)(scr.top + mb + 19 + 2), 0, 0);
    r.right = (short)(r.left + W);
    r.bottom = (short)(r.top + H);
    win = NewWindow(NULL, &r, "\pShuttle", true, noGrowDocProc, (WindowPtr)-1, true, 0);
    SetPort(win);
    dbg.winTop = r.top;
    dbg.winLeft = r.left;
    dbg.winH = H;
    dbg.winW = W;
    SetRect(&statusR, M + 98, M + 2, W - M, M + 20);
    SetRect(&pathR, M, 36, W - M - 64, 54);
    SetRect(&listBox, M, 58, W - M, (short)(H - 74));
    SetRect(&sbR, (short)(W - M - 16), 58, W - M, (short)(H - 74));
    SetRect(&headR, M + 1, 59, (short)(sbR.left), 73);
    visRows = (short)((listBox.bottom - 1 - 74) / ROW_H);
    SetRect(&rowsR, M + 1, 74, sbR.left, (short)(74 + visRows * ROW_H));
    colSize = (short)(rowsR.right - 96);
    colKind = (short)(rowsR.right - 86);
    SetRect(&freeR, M + 210, (short)(H - 66), W - M, (short)(H - 52));
    SetRect(&msgR, M, (short)(H - 44), W - M, (short)(H - 30));
    SetRect(&barR, M, (short)(H - 26), (short)(W - M - 82), (short)(H - 14));
    SetRect(&r, M, M, M + 86, M + 20);
    bConnect = NewControl(win, &r, "\pConnect", true, 0, 0, 1, pushButProc, 1);
    SetRect(&r, (short)(W - M - 56), 35, W - M, 53);
    bUp = NewControl(win, &r, "\pUp", true, 0, 0, 1, pushButProc, 2);
    SetRect(&r, M, (short)(H - 70), M + 92, (short)(H - 50));
    bDownload = NewControl(win, &r, "\pDownload", true, 0, 0, 1, pushButProc, 3);
    SetRect(&r, M + 100, (short)(H - 70), M + 200, (short)(H - 50));
    bSend = NewControl(win, &r, "\pSend File\311", true, 0, 0, 1, pushButProc, 4);
    SetRect(&r, (short)(W - M - 72), (short)(H - 30), W - M, (short)(H - 10));
    bCancel = NewControl(win, &r, "\pCancel", true, 0, 0, 1, pushButProc, 5);
    sbar = NewControl(win, &sbR, "\p", true, 0, 0, 0, scrollBarProc, 6);
}

/* ------------------------------------------------------------------ prefs */
static void defaultPrefs(void)
{
    memset(&prefs, 0, sizeof prefs);
    prefs.magic = PREFS_MAGIC;
    prefs.version = 1;
    prefs.baud = 3;
    prefs.flow = FLOW_NONE;
    pset(prefs.dial, "mac", 63);
    pset(prefs.init, "ATE0V1", 63);
}

static short prefsVol(void)
{
    SysEnvRec env;
    if (SysEnvirons(1, &env) == noErr) return env.sysVRefNum;
    return 0;
}

static void loadPrefs(void)
{
    short ref;
    long n = sizeof prefs;
    Prefs p;
    defaultPrefs();
    if (FSOpen("\pShuttle Prefs", prefsVol(), &ref) != noErr) return;
    if (FSRead(ref, &n, &p) == noErr && n == (long)sizeof p && p.magic == PREFS_MAGIC && p.version == 1) prefs = p;
    FSClose(ref);
    if (prefs.baud < 0 || prefs.baud > BAUD_MAMETEST) prefs.baud = 3;
}

static void savePrefs(void)
{
    short ref, vol = prefsVol();
    long n = sizeof prefs;
    OSErr e;
    prefsDirty = 0;
    e = FSOpen("\pShuttle Prefs", vol, &ref);
    if (e == fnfErr) {
        e = Create("\pShuttle Prefs", vol, 'Shtl', 'pref');
        if (e == noErr) e = FSOpen("\pShuttle Prefs", vol, &ref);
    }
    if (e == noErr) {
        e = FSWrite(ref, &n, &prefs);
        SetEOF(ref, sizeof prefs);
        FSClose(ref);
        FlushVol(NULL, vol);
    }
    if (e != noErr && !prefsSaveFailed) {
        prefsSaveFailed = 1;
        strcpy(msg, "(Settings couldn't be saved: is the disk locked? They last until you quit.)");
        SetPort(win);
        InvalRect(&msgR);
    }
}

/* ------------------------------------------------------------------ hooks from link.c and proto.c */
static void alertText(const char *s)
{
    Str255 p;
    pset(p, s, 255);
    ParamText(p, "\p", "\p", "\p");
    InitCursor();
    StopAlert(131, NULL);
}

void ui_error(short code, const char *s)
{
    dbg.errors++;
    dbg.lastErr = code;
    setDbgStr(dbg.msg, s);
    strncpy(msg, s, sizeof msg - 1);
    if (win) { SetPort(win); InvalRect(&msgR); }
    alertText(s);
}

void ui_stateChanged(void)
{
    dbg.linkState = linkState;
    dbg.pState = pState;
    dbg.pJob = pJob;
    if (!win) return;
    if (linkState == L_OFF && nItems) {
        nItems = 0;
        sel = -1;
        path[0] = 0;
        dbg.items = 0;
        SetPort(win);
        InvalRect(&win->portRect);
    }
    updateButtons();
    updateMenus();
    drawStatus();
    drawPath();
}

void ui_connected(void)
{
    path[0] = 0;
    requestList("");
}

void ui_note(const char *s)
{
    strncpy(note, s, sizeof note - 1);
    note[sizeof note - 1] = 0;
    noteTick = TickCount();
    drawStatus();
}

static void finishedText(short job, const char *savedAs)
{
    char n[16];
    long t = (long)(pEnd - pStart), rate = t > 0 ? pTotal * 60 / t : 0;
    if (t < 1) t = 1;
    if (job == J_GET) {
        strcpy(msg, "Saved \"");
        p2c(xferName, msg + strlen(msg), 40);
        strcat(msg, "\" (");
    } else {
        strcpy(msg, "Sent \"");
        p2c(xferName, msg + strlen(msg), 40);
        strcat(msg, "\" to From Plus");
        if (savedAs[0] && (savedAs[0] != xferName[1] || strlen(savedAs) != xferName[0])) {
            strcat(msg, " as \"");
            strncat(msg, savedAs, 32);
            strcat(msg, "\"");
        }
        strcat(msg, " (");
    }
    sizeStr(pTotal, n);
    strcat(msg, n);
    strcat(msg, ") in ");
    timeStr((t + 30) / 60, n);
    strcat(msg, n);
    strcat(msg, ", ");
    numStr(rate, n, 1);
    strcat(msg, n);
    strcat(msg, " bytes/s.");
    dbg.bps = rate;
    dbg.ticks = t;
}

void ui_transferDone(short job, const char *savedAs)
{
    if (job == J_GET) dbg.downloads++;
    if (job == J_PUT) dbg.uploads++;
    finishedText(job, savedAs);
    SetPort(win);
    InvalRect(&msgR);
    InvalRect(&barR);
    drawFree();
    if (job == J_PUT && (!path[0] || !strcmp(path, "From Plus"))) requestList(path);   /* show it */
    SysBeep(1);
}

void ui_transferFailed(short job, short code, const char *s)
{
    if (code == E_CANCEL) dbg.cancels++;
    if (job == J_LIST) strcpy(wantPath, path);
    strncpy(msg, s, sizeof msg - 1);
    msg[sizeof msg - 1] = 0;
    setDbgStr(dbg.msg, s);
    if (win) {
        SetPort(win);
        InvalRect(&msgR);
        InvalRect(&barR);
        drawFree();
    }
    if (code != E_CANCEL) {
        dbg.errors++;
        dbg.lastErr = code;
        if (code != E_LOST || linkState == L_ONLINE) alertText(s);
    }
}

/* ------------------------------------------------------------------ download and send */
static void doDownload(void)
{
    SFReply reply;
    Point where;
    Item *it;
    long alb, f, need, old;
    OSErr e;
    char p[256];
    if (!proto_ready() || sel < 0 || sel >= nItems) return;
    it = &items[sel];
    if (it->kind != 'F') { openItem(); return; }
    if (firstSave && startVol) {                     /* the first time: the startup disk's top folder */
        LMSetSFSaveDisk((short)-startVol);
        LMSetCurDirStore(2);
        firstSave = 0;
    }
    where.h = (short)((qd.screenBits.bounds.left + qd.screenBits.bounds.right - 304) / 2);
    where.v = (short)(qd.screenBits.bounds.top + 70);
    SFPutFile(where, "\pSave the download as:", it->name, NULL, &reply);
    SetPort(win);
    if (!reply.good) return;
    f = vol_free(reply.vRefNum, &alb);
    if (alb < 512) alb = 512;
    need = (it->dlen + alb - 1) / alb * alb + (it->rlen + alb - 1) / alb * alb;
    old = file_size(reply.fName, reply.vRefNum);
    if (f >= 0 && need > f + (old > 0 ? old : 0)) {
        char s[200], n[16];
        strcpy(s, "There isn't room for it: \"");
        p2c(it->name, s + strlen(s), 32);
        strcat(s, "\" needs ");
        sizeStr(need, n);
        strcat(s, n);
        strcat(s, ", and that disk has ");
        sizeStr(f, n);
        strcat(s, n);
        strcat(s, " free.");
        ui_error(E_DISK, s);
        return;
    }
    e = dl_begin(reply.fName, reply.vRefNum, it->type, it->creator, it->dlen, it->rlen);
    if (e != noErr) { ui_error(E_DISK, files_errText(e)); return; }
    strcpy(p, path);
    if (p[0]) strcat(p, ":");
    {
        short k = (short)strlen(p);
        memcpy(p + k, it->name + 1, it->name[0]);
        p[k + it->name[0]] = 0;
    }
    memcpy(xferName, reply.fName, reply.fName[0] + 1);
    xferJob = J_GET;
    if (!proto_get(p, it->dlen, it->rlen)) { dl_abort(); return; }
    InvalRect(&msgR);
}

static void doSend(void)
{
    SFReply reply;
    Point where;
    long total;
    OSErr e;
    if (!proto_ready()) return;
    where.h = (short)((qd.screenBits.bounds.left + qd.screenBits.bounds.right - 348) / 2);
    where.v = (short)(qd.screenBits.bounds.top + 50);
    SFGetFile(where, "\p", NULL, -1, NULL, NULL, &reply);
    SetPort(win);
    if (!reply.good) return;
    e = ul_begin(reply.fName, reply.vRefNum, &total);
    if (e != noErr) { ui_error(E_FILE, files_errText(e)); return; }
    memcpy(xferName, reply.fName, reply.fName[0] + 1);
    xferJob = J_PUT;
    if (!proto_put(reply.fName, total)) { ul_end(); return; }
    InvalRect(&msgR);
}

static void doCancel(void)
{
    if (!proto_busy()) return;
    proto_cancel();
}

static void doConnect(void)
{
    if (linkState == L_OFF) {
        msg[0] = 0;
        link_connect();
    } else if (linkState != L_TERM) proto_disconnect();
}

/* ------------------------------------------------------------------ the modem terminal */
static TEHandle termTE;
static char termIn[200];
static short termInLen;
static Rect termView, termInR;

static void termAppend(const char *s, short n)
{
    if (!termTE) return;
    if ((*termTE)->teLength > 6000) {
        TESetSelect(0, 2000, termTE);
        TEDelete(termTE);
    }
    TESetSelect(32767, 32767, termTE);
    TEInsert(s, n, termTE);
    TESelView(termTE);
}

static void termDrawInput(void)
{
    GrafPtr old;
    if (!termWin) return;
    GetPort(&old);
    SetPort(termWin);
    EraseRect(&termInR);
    MoveTo(termInR.left, (short)(termInR.top - 3));
    LineTo(termInR.right, (short)(termInR.top - 3));
    TextFont(4);
    TextSize(9);
    MoveTo((short)(termInR.left + 4), (short)(termInR.bottom - 5));
    DrawText("> ", 0, 2);
    {
        short start = 0;
        while (start < termInLen && TextWidth(termIn, start, (short)(termInLen - start)) > termInR.right - termInR.left - 30)
            start++;
        DrawText(termIn, start, (short)(termInLen - start));
    }
    DrawChar('_');
    SetPort(old);
}

static void openTerminal(void)
{
    Rect r, scr = qd.screenBits.bounds;
    if (!termWin) {
        short w = (short)(scr.right - scr.left - 60 > 400 ? 400 : scr.right - scr.left - 60);
        SetRect(&r, (short)(scr.left + (scr.right - scr.left - w) / 2), 70, 0, 260);
        r.right = (short)(r.left + w);
        termWin = NewWindow(NULL, &r, "\pModem Terminal", true, noGrowDocProc, (WindowPtr)-1, true, 0);
        SetPort(termWin);
        TextFont(4);
        TextSize(9);
        SetRect(&termView, 4, 4, (short)(w - 4), 166);
        SetRect(&termInR, 0, 172, w, 190);
        termTE = TENew(&termView, &termView);
        TEAutoView(true, termTE);
        termInLen = 0;
    } else SelectWindow(termWin);
    if (linkState == L_OFF) {
        link_terminal();
        if (linkState == L_TERM) {
            const char *s = "Typed lines go straight to the modem (try AT, then ATI or AT+HELP). Type +++ alone to get "
                            "back to its commands. Close this window to end.\r";
            termAppend(s, (short)strlen(s));
        }
    } else if (linkState != L_TERM) {
        const char *s = "(The modem is busy with the connection. Disconnect first to talk to it.)\r";
        termAppend(s, (short)strlen(s));
    }
}

static void closeTerminal(void)
{
    if (!termWin) return;
    TEDispose(termTE);
    termTE = NULL;
    DisposeWindow(termWin);
    termWin = NULL;
    if (linkState == L_TERM) link_disconnect();
}

void ui_termText(const char *s, short n)
{
    char out[256];
    short i, k = 0;
    GrafPtr old;
    if (!termWin) return;
    GetPort(&old);
    SetPort(termWin);
    for (i = 0; i < n && k < 255; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\r') out[k++] = '\r';
        else if (c >= 32 && c != 127) out[k++] = (char)c;
    }
    if (k) termAppend(out, k);
    SetPort(old);
}

static void termKey(char c)
{
    if (c == '\r' || c == 3) {
        char echo[210];
        if (linkState != L_TERM) { SysBeep(5); return; }
        echo[0] = '>';
        echo[1] = ' ';
        memcpy(echo + 2, termIn, termInLen);
        echo[2 + termInLen] = '\r';
        SetPort(termWin);
        termAppend(echo, (short)(termInLen + 3));
        link_termSend(termIn, termInLen);
        termInLen = 0;
    } else if (c == 8) {
        if (termInLen) termInLen--;
    } else if ((unsigned char)c >= 32 && c != 127 && termInLen < (short)sizeof termIn - 1) termIn[termInLen++] = c;
    termDrawInput();
}

/* ------------------------------------------------------------------ settings */
enum { SI_OK = 1, SI_CANCEL, SI_DEFAULTS, SI_DIAL = 5, SI_INIT = 7, SI_BAUD = 9, SI_FLOW = 16, SI_PORT = 20,
       SI_AUTO = 22, SI_OUTLINE = 24 };

static pascal void drawOutline(DialogPtr d, short item)
{
    short t;
    Handle h;
    Rect r;
    PenState ps;
    GetDialogItem(d, item, &t, &h, &r);
    GetPenState(&ps);
    PenSize(3, 3);
    FrameRoundRect(&r, 16, 16);
    SetPenState(&ps);
}

static pascal Boolean settingsFilter(DialogPtr d, EventRecord *ev, short *item)
{
    (void)d;
    if (ev->what == keyDown || ev->what == autoKey) {
        char c = (char)(ev->message & charCodeMask);
        if (c == '\r' || c == 3) { *item = SI_OK; return true; }
        if ((ev->modifiers & cmdKey) && c == '.') { *item = SI_CANCEL; return true; }
    }
    return false;
}

static void setCtl(DialogPtr d, short item, short v)
{
    short t;
    Handle h;
    Rect r;
    GetDialogItem(d, item, &t, &h, &r);
    SetControlValue((ControlHandle)h, v);
}

static void setText(DialogPtr d, short item, const unsigned char *p)
{
    short t;
    Handle h;
    Rect r;
    GetDialogItem(d, item, &t, &h, &r);
    SetDialogItemText(h, p);
}

static void getText(DialogPtr d, short item, unsigned char *p, short max)
{
    short t;
    Handle h;
    Rect r;
    Str255 s;
    GetDialogItem(d, item, &t, &h, &r);
    GetDialogItemText(h, s);
    if (s[0] > max) s[0] = (unsigned char)max;
    memcpy(p, s, s[0] + 1);
}

static void setRadios(DialogPtr d, const Prefs *p)
{
    short i;
    for (i = 0; i < NBAUDS; i++) setCtl(d, (short)(SI_BAUD + i), i == p->baud);
    for (i = 0; i < 3; i++) setCtl(d, (short)(SI_FLOW + i), i == p->flow);
    for (i = 0; i < 2; i++) setCtl(d, (short)(SI_PORT + i), i == p->port);
    setCtl(d, SI_AUTO, p->autoConnect);
}

static void doSettings(void)
{
    DialogPtr d = GetNewDialog(200, NULL, (WindowPtr)-1);
    Prefs p = prefs, old = prefs;
    short item = 0, t, w, h;
    Handle hd;
    Rect r, scr = qd.screenBits.bounds;
    if (!d) { SysBeep(5); return; }
    w = (short)(d->portRect.right - d->portRect.left);
    h = (short)(d->portRect.bottom - d->portRect.top);
    MoveWindow(d, (short)((scr.left + scr.right - w) / 2), (short)(LMGetMBarHeight() + (scr.bottom - LMGetMBarHeight() - h) / 3),
               false);
    GetDialogItem(d, SI_OUTLINE, &t, &hd, &r);
    SetDialogItem(d, SI_OUTLINE, userItem, (Handle)drawOutline, &r);
    setText(d, SI_DIAL, p.dial);
    setText(d, SI_INIT, p.init);
    setRadios(d, &p);
    SelectDialogItemText(d, SI_DIAL, 0, 32767);
    ShowWindow(d);
    for (;;) {
        ModalDialog((ModalFilterUPP)settingsFilter, &item);
        if (item == SI_OK || item == SI_CANCEL) break;
        if (item == SI_DEFAULTS) {
            Prefs keep = prefs;
            defaultPrefs();
            p = prefs;
            prefs = keep;
            setText(d, SI_DIAL, p.dial);
            setText(d, SI_INIT, p.init);
            setRadios(d, &p);
            continue;
        }
        if (item >= SI_BAUD && item < SI_BAUD + NBAUDS) p.baud = (short)(item - SI_BAUD);
        else if (item >= SI_FLOW && item < SI_FLOW + 3) p.flow = (short)(item - SI_FLOW);
        else if (item >= SI_PORT && item < SI_PORT + 2) p.port = (short)(item - SI_PORT);
        else if (item == SI_AUTO) p.autoConnect = (short)!p.autoConnect;
        setRadios(d, &p);
    }
    if (item == SI_OK) {
        getText(d, SI_DIAL, p.dial, 63);
        getText(d, SI_INIT, p.init, 63);
        prefs = p;
        prefsDirty = 1;
    }
    DisposeDialog(d);
    SetPort(win);
    if (item == SI_OK && linkState != L_OFF && memcmp(&old, &prefs, sizeof prefs)) {
        strcpy(msg, "(The new settings apply the next time you connect.)");
        InvalRect(&msgR);
    }
}

/* ------------------------------------------------------------------ menus */
static void quitApp(void)
{
    unsigned long t0 = TickCount();
    EventRecord ev;
    if (proto_busy()) proto_cancel();
    if (linkState == L_TERM) link_disconnect();
    if (linkState != L_OFF) {
        proto_disconnect();
        while (linkState != L_OFF && TickCount() - t0 < 60 * 8) {
            link_poll();
            proto_poll();
            SystemTask();
            GetNextEvent(updateMask, &ev);
        }
    }
    ser_close();
    if (prefsDirty) savePrefs();
    ExitToShell();
}

static void doMenu(long mr)
{
    short menu = (short)(mr >> 16), item = (short)mr;
    if (menu == 128) {
        if (item == 1) { InitCursor(); Alert(130, NULL); }
        else if (item > 2) {
            Str255 name;
            GetMenuItemText(appleM, item, name);
            OpenDeskAcc(name);
            SetPort(win);
        }
    } else if (menu == 129) {
        if (item == 1) doDownload();
        else if (item == 2) doSend();
        else if (item == 3) doCancel();
        else if (item == 5) quitApp();
    } else if (menu == 130) {
        SystemEdit((short)(item - 1));
    } else if (menu == 131) {
        if (item == 1) doConnect();
        else if (item == 2) requestList(path);
        else if (item == 4) openTerminal();
        else if (item == 5) doSettings();
    }
    HiliteMenu(0);
}

/* ------------------------------------------------------------------ mouse and keys */
static pascal void scrollAction(ControlHandle c, short part)
{
    short d = 0;
    if (part == inUpButton) d = -1;
    else if (part == inDownButton) d = 1;
    else if (part == inPageUp) d = (short)-(visRows - 1);
    else if (part == inPageDown) d = (short)(visRows - 1);
    if (d) {
        scrollTo((short)(topRow + d));
        SetControlValue(c, topRow);
    }
}

static void contentClick(EventRecord *ev)
{
    ControlHandle c;
    Point pt = ev->where;
    short part;
    SetPort(win);
    GlobalToLocal(&pt);
    part = FindControl(pt, win, &c);
    if (c == sbar && part) {
        if (part == inThumb) {
            if (TrackControl(c, pt, NULL)) scrollTo(GetControlValue(c));
        } else TrackControl(c, pt, (ControlActionUPP)scrollAction);
        return;
    }
    if (c && part) {
        if (!TrackControl(c, pt, NULL)) return;
        if (c == bConnect) doConnect();
        else if (c == bUp) goUp();
        else if (c == bDownload) doDownload();
        else if (c == bSend) doSend();
        else if (c == bCancel) doCancel();
        return;
    }
    if (PtInRect(pt, &rowsR)) {
        short row = (short)(topRow + (pt.v - rowsR.top) / ROW_H);
        if (row >= nItems) return;
        if (row == lastClickRow && ev->when - lastClickTick < (unsigned long)*(long *)0x2F0) {
            select(row);
            lastClickRow = -1;
            doDownload();                        /* opens a folder, downloads a file */
            return;
        }
        select(row);
        lastClickRow = row;
        lastClickTick = ev->when;
    }
}

static void doMouse(EventRecord *ev)
{
    WindowPtr w;
    short part = FindWindow(ev->where, &w);
    if (part == inMenuBar) { updateMenus(); doMenu(MenuSelect(ev->where)); }
    else if (part == inSysWindow) SystemClick(ev, w);
    else if (part == inDrag) {
        Rect lim = qd.screenBits.bounds;
        InsetRect(&lim, 4, 4);
        DragWindow(w, ev->where, &lim);
    } else if (part == inGoAway) {
        if (TrackGoAway(w, ev->where)) {
            if (w == termWin) closeTerminal();
            else if (w == win) quitApp();
        }
    } else if (part == inContent) {
        if (w != FrontWindow()) SelectWindow(w);
        else if (w == win) contentClick(ev);
    }
}

static void doKey(EventRecord *ev)
{
    char c = (char)(ev->message & charCodeMask);
    short code = (short)((ev->message & keyCodeMask) >> 8);
    if (ev->modifiers & cmdKey) {
        if (c == '.') { doCancel(); return; }
        if (c == 30 || code == 0x4D) { goUp(); return; }  /* Cmd-Up: the folder above */
        updateMenus();
        doMenu(MenuKey(c));
        return;
    }
    if (FrontWindow() == termWin && termWin) { termKey(c); return; }
    if (c == 30 || code == 0x4D) { select((short)(sel - 1)); return; }      /* up */
    if (c == 31 || code == 0x48) { select((short)(sel + 1)); return; }      /* down */
    if (c == 28 || code == 0x46) { goUp(); return; }                        /* left: the folder above */
    if (c == 29 || code == 0x42) { if (sel >= 0 && items[sel].kind != 'F') openItem(); return; }   /* right */
    if (c == '\r' || c == 3) { doDownload(); return; }
    if ((unsigned char)c > 32 || (c == ' ' && typedLen)) {                   /* type to select */
        short i;
        if (TickCount() - typedTick > 60) typedLen = 0;
        typedTick = TickCount();
        if (typedLen < (short)sizeof typed - 1) typed[typedLen++] = c;
        for (i = 0; i < nItems; i++) {
            short k;
            if (items[i].name[0] < typedLen) continue;
            for (k = 0; k < typedLen && lower(items[i].name[1 + k]) == lower(typed[k]); k++) {}
            if (k == typedLen) { select(i); break; }
        }
    }
}

/* ------------------------------------------------------------------ main */
int main(void)
{
    EventRecord ev;
    static char lastStatus[120];

    dbg.magic = MAGIC;
    dbg.version = 1;
    dbg.notMagic = ~MAGIC;
    dbg.sel = -1;

    MaxApplZone();
    MoreMasters();
    MoreMasters();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);

    appleM = GetMenu(128);
    AppendResMenu(appleM, 'DRVR');
    InsertMenu(appleM, 0);
    fileM = GetMenu(129);
    InsertMenu(fileM, 0);
    editM = GetMenu(130);
    InsertMenu(editM, 0);
    connM = GetMenu(131);
    InsertMenu(connM, 0);
    DrawMenuBar();

    link_init();
    items = (Item *)NewPtr(MAXITEMS * sizeof(Item));
    startVol = startup_vol();
    loadPrefs();
    layout();
    strcpy(msg, "Click Connect to reach the MacBook (the Shuttle server must be running there).");
    ui_stateChanged();
    drawAll();
    ValidRect(&win->portRect);
    dbg.freeMem = FreeMem();
    if (prefs.autoConnect) link_connect();

    for (;;) {
        unsigned long now;
        SystemTask();
        link_poll();
        proto_poll();
        now = TickCount();
        dbg.linkState = linkState;
        dbg.pState = pState;
        dbg.pJob = pJob;
        dbg.rx = linkRx;
        dbg.tx = linkTx;
        dbg.done = pDone;
        dbg.total = pTotal;
        dbg.serErr = ser_errors();
        dbg.naks = stNaks;
        dbg.timeouts = stTimeouts;
        dbg.flushes = stFlushes;
        dbg.badFrames = linkBadFrames;
        dbg.resends = stResends;
        dbg.blocks = stBlocks;
        memcpy(dbg.ser, serDbg, sizeof dbg.ser);
        dbg.dlCrc[0] = dlCrc[0];
        dbg.dlCrc[1] = dlCrc[1];
        if (now - lastDraw > 20) {
            char s[120];
            statusText(s);
            SetPort(win);
            if (strcmp(s, lastStatus)) { strcpy(lastStatus, s); drawStatus(); }
            if (proto_busy()) drawProgress();
            else lastDraw = now;
        }
        if (prefsDirty && (linkState == L_OFF || proto_ready())) savePrefs();
        if (!GetNextEvent(everyEvent, &ev)) continue;
        switch (ev.what) {
        case keyDown:
        case autoKey:
            doKey(&ev);
            break;
        case mouseDown:
            doMouse(&ev);
            break;
        case updateEvt:
            BeginUpdate((WindowPtr)ev.message);
            if ((WindowPtr)ev.message == win) drawAll();
            else if ((WindowPtr)ev.message == termWin && termTE) {
                SetPort(termWin);
                EraseRect(&termWin->portRect);
                TEUpdate(&termWin->portRect, termTE);
                termDrawInput();
            }
            EndUpdate((WindowPtr)ev.message);
            break;
        default:
            break;
        }
    }
    return 0;
}

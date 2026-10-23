/*
 * Pixel: a tiny chatbot for the Macintosh Plus / SE (System 6 and 7).
 *
 * Loads the model ("Pixel Model 1" next to the app, "Pixel Model 2" there or on any mounted disk;
 * if missing, ejects the boot floppy and asks for Pixel Disk 2), picks W4 or B5 storage from the
 * free memory, then runs the chat full screen (under the menu bar, whatever width screenBits has):
 * type a line, press Return, Pixel answers token by token. "reset" clears Pixel's memory.
 *
 * Test mode: a text file "Pixel Test" next to the app holds "seed temp256 topk max_new" and then one
 * user line per line; the app types them itself, logs every sampled token, and fills the debug block
 * below (magic 'PXL!') for a MAME Lua script. llm/ref/chat.py is the bit-exact reference.
 */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <OSUtils.h>
#include <MacMemory.h>
#include <Files.h>
#include <ToolUtils.h>
#include <LowMem.h>
#include <string.h>
#include "engine.h"

#define MAGIC 0x50584C21UL     /* 'PXL!' */
#define TEMP_DEFAULT 205       /* 0.8 */
#define TOPK_DEFAULT 16
#define MAXNEW_DEFAULT 32      /* reply cap in tokens: about 25 words, the corpus's 1-2 sentences */
#define LOGMAX 2048

struct Dbg {
    unsigned long magic, version, notMagic;   /*  0,  4,  8 */
    long state;          /* 12: 0 start, 1 loading, 2 ready, 3 busy, 4 test done, <0 error */
    long variant;        /* 16: 0 W4, 1 B5 */
    long loadErr;        /* 20 */
    unsigned long loadTicks;  /* 24 */
    long maxBlock;       /* 28: MaxBlock before loading */
    long freeAfter;      /* 32: FreeMem after loading */
    long ntok;           /* 36: tokens in the log */
    unsigned long genTicks;   /* 40: ticks in steps that computed logits */
    long genSteps;       /* 44 */
    unsigned long ingTicks;   /* 48: ticks in steps without logits */
    long ingSteps;       /* 52 */
    long kself;          /* 56: kernel self-test mismatches (-1 = not run) */
    unsigned long logAddr;    /* 60: int16 token log */
    long swaps;          /* 64: "insert disk 2" prompts */
    long turns;          /* 68 */
    unsigned long seed;  /* 72 */
    unsigned long readTicks;  /* 76: ticks inside FSRead while loading */
    long selftest;       /* 80: 1 if this is the test build (kernel self-test included) */
    unsigned long prof[PP_N]; /* 84: test build: ticks per phase (norm, tern, att, attk, ffn, head, sample) */
    unsigned long lprof[PL_N];/* 140: test build: load ticks (embedding decode, ternary decode, expand, sort) */
    long kbad[8];             /* 156: self-test mismatches: apply16c, dot8, scores, softp, softw, rshr, other */
    long minFree;             /* 188: SE builds: lowest FreeMem seen while loading (bytes) */
    long kbTicks[4];          /* 192: PixelSE: ticks of the last keyboard draw and the last full redraw */
    short geo[16];            /* 208: PixelSE layout for test scripts: kbArea, kbButton, inputBox, area (t,l,b,r) */
};
static struct Dbg dbg;
static int16_t tokLog[LOGMAX];

static PxModel M;
static WindowPtr win;
static int temp256 = TEMP_DEFAULT, topk = TOPK_DEFAULT, maxNew = MAXNEW_DEFAULT;

/* ------------------------------------------------------------------ busy cursor (no resources) */
static Cursor hourglass = {
    {0x3FFC, 0x3FFC, 0x2004, 0x1008, 0x0BD0, 0x05A0, 0x0240, 0x0240,
     0x0240, 0x0420, 0x0990, 0x13C8, 0x27E4, 0x2FF4, 0x3FFC, 0x3FFC},
    {0x3FFC, 0x3FFC, 0x3FFC, 0x1FF8, 0x0FF0, 0x07E0, 0x03C0, 0x03C0,
     0x03C0, 0x07E0, 0x0FF0, 0x1FF8, 0x3FFC, 0x3FFC, 0x3FFC, 0x3FFC},
    {8, 8}};

/* ------------------------------------------------------------------ full-screen layout
 * One borderless window covers the whole screen (qd.screenBits.bounds: Narrow Screen narrows it on
 * the Plus), so local coordinates are global ones. Under the menu bar, top to bottom:
 *   header   icon, "Pixel", status at the right; double line below
 *   area     the transcript: "You" / "Pixel" labels in a column, word-wrapped text beside them
 *   inBar    double line, then the input field (and, PixelSE, the Keys button)
 *   kbArea   PixelSE: the on-screen keyboard, docked full width at the bottom (when open)
 */
#define HDR_H 21            /* header: 18 px strip + double line */
#define INBAR_H 29          /* input bar: double line + 20 px field */
#define MARGIN 10           /* left and right margin of the text */
#define LABGAP 8            /* between the label column and the text */
#define ROWS_TOP 6          /* first transcript row, below the header */
#define KEYS_W 54           /* the Keys button */
static Rect scr, hdrBox, area, inBar, inputBox;
static short lineH = 16, ascent = 12, visLines, bodyFont = 3;
static short labR, textX, textR;     /* right edge of the labels; left and right edge of message text */

static void hline(const Rect *r, short dy)          /* a line across r, dy below its top */
{
    MoveTo(r->left, (short)(r->top + dy));
    LineTo((short)(r->right - 1), (short)(r->top + dy));
}

/* ------------------------------------------------------------------ transcript (with scrollback) */
#define MAXROWS 48          /* rows kept; the newest visLines of them are on screen */
#define LINELEN 126         /* a Row is 128 bytes */
enum { RK_TEXT, RK_YOU, RK_PIXEL, RK_NOTE };   /* continuation, first row of a turn, centred note */
typedef struct { unsigned char len, kind; char t[LINELEN]; } Row;
static Row rows[MAXROWS], *cur;    /* cur: the last row, where text goes */
static short nRows, rowW;           /* rows in the history; pixel width of the last row */
static char input[200];
static short inLen;

/* ------------------------------------------------------------------ on-screen keyboard (mouse only) */
#ifdef PX_KB    /* PixelSE only: the lab's SE has no keyboard; the Plus has one */
#define KB_MX 6             /* side margin */
#define KB_GAP 5            /* between keys */
#define KB_TOP 5            /* above the first row */
#define KB_KH 18            /* key height (a 1 px shadow below) */
#define KB_PITCH 22         /* row pitch */
#define KB_H (KB_TOP + 5 * KB_PITCH + 3)   /* + a margin above the screen's rounded bottom corners */
static short kbOpen, kbCaps;
static Rect kbArea, kbButton;
static const char kbRows[4][11] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?"};
static const char kbBot[7] = {1, '!', '-', ' ', 8, '\r', 2};            /* 1 caps, 2 close */
static const unsigned short kbCum[8] = {0, 48, 78, 108, 228, 280, 332, 384};   /* bottom row, of 384 */
static const char kbBotL[7][7] = {"Shift", "!", "-", "space", "Delete", "Send", "Close"};

/* key k (0-46) of the keyboard: its rectangle and character. The 4 x 10 keys and the bottom row
   share the width (kbCum: the bottom row's key edges, in 384ths). */
static char kbKey(short k, Rect *r)
{
    long w = kbArea.right - kbArea.left - 2 * KB_MX + KB_GAP;
    short x0 = (short)(kbArea.left + KB_MX), a, b, top;
    if (k < 40) {
        a = (short)(w * (k % 10) / 10);
        b = (short)(w * (k % 10 + 1) / 10);
        top = (short)(kbArea.top + KB_TOP + (k / 10) * KB_PITCH);
    } else {
        a = (short)(w * kbCum[k - 40] / 384);
        b = (short)(w * kbCum[k - 39] / 384);
        top = (short)(kbArea.top + KB_TOP + 4 * KB_PITCH);
    }
    SetRect(r, (short)(x0 + a), top, (short)(x0 + b - KB_GAP), (short)(top + KB_KH));
    return k < 40 ? kbRows[k / 10][k % 10] : kbBot[k - 40];
}

static void drawKey(short k)
{
    Rect r;
    char c = kbKey(k, &r), lab[2];
    const char *l = lab;
    short n = 1;
    EraseRoundRect(&r, 8, 8);
    FrameRoundRect(&r, 8, 8);
    MoveTo((short)(r.left + 3), r.bottom);              /* shadow: the key stands up */
    LineTo((short)(r.right - 4), r.bottom);
    if (k >= 40 && kbBotL[k - 40][1]) { l = kbBotL[k - 40]; n = (short)strlen(l); }
    else lab[0] = (char)(kbCaps && c >= 'a' && c <= 'z' ? c - 32 : c);
    MoveTo((short)((r.left + r.right - TextWidth(l, 0, n)) / 2), (short)(r.bottom - 5));
    DrawText(l, 0, n);
    if (c == 1 && kbCaps) { InsetRect(&r, 1, 1); InvertRoundRect(&r, 6, 6); }
}

static void drawKeysDirect(void)
{
    short k;
    EraseRect(&kbArea);
    hline(&kbArea, 0);
    TextFont(0);
    for (k = 0; k < 47; k++) drawKey(k);
    TextFont(bodyFont);
}

/* The keyboard is drawn once per case into an offscreen image (about 7.5 KB each; kbArea never
   moves), then copied: 47 round-rect keys with labels take about 46 ticks, the copy a few. */
static Ptr kbBits[2];

static void drawKeys(void)
{
    BitMap bm;
    unsigned long t0 = TickCount();
    bm.rowBytes = (short)(((kbArea.right - kbArea.left + 15) >> 4) << 1);
    bm.bounds = kbArea;
    if (!kbBits[kbCaps] && (bm.baseAddr = NewPtr((long)bm.rowBytes * (kbArea.bottom - kbArea.top))) != NULL) {
        GrafPort off;
        OpenPort(&off);                     /* a port on the image, in the window's coordinates */
        SetPortBits(&bm);
        off.portRect = kbArea;
        RectRgn(off.visRgn, &kbArea);
        TextSize(12);
        drawKeysDirect();
        ClosePort(&off);
        SetPort(win);
        kbBits[kbCaps] = bm.baseAddr;
    }
    if ((bm.baseAddr = kbBits[kbCaps]) != NULL) CopyBits(&bm, &win->portBits, &kbArea, &kbArea, srcCopy, NULL);
    else drawKeysDirect();                  /* no memory for the image */
    dbg.kbTicks[0] = TickCount() - t0;
}
#endif /* PX_KB */

/* ------------------------------------------------------------------ header and status */
static const unsigned short pxIcon[16] = {   /* the little Mac of the Finder icon, 16 x 16 */
    0x3FFC, 0x4002, 0x5FFA, 0x500A, 0x524A, 0x524A, 0x500A, 0x542A,
    0x53CA, 0x500A, 0x5FFA, 0x4002, 0x4C72, 0x7FFE, 0x4002, 0x7FFE};

static void drawStatus(const char *s)
{
    Rect r = hdrBox;
    short n = (short)strlen(s);
    r.left = (short)(r.right - 120);
    r.bottom = (short)(r.top + HDR_H - 3);
    EraseRect(&r);
    TextFont(3);
    TextSize(9);
    MoveTo((short)(hdrBox.right - MARGIN - TextWidth(s, 0, n)), (short)(hdrBox.top + 12));
    DrawText(s, 0, n);
    TextFont(bodyFont);
    TextSize(12);
}

static void drawHeader(void)
{
    BitMap bm;
    Rect d;
    bm.baseAddr = (Ptr)pxIcon;
    bm.rowBytes = 2;
    SetRect(&bm.bounds, 0, 0, 16, 16);
    SetRect(&d, (short)(hdrBox.left + MARGIN - 2), (short)(hdrBox.top + 1), (short)(hdrBox.left + MARGIN + 14),
            (short)(hdrBox.top + 17));
    CopyBits(&bm, &win->portBits, &bm.bounds, &d, srcCopy, NULL);
    TextFont(0);
    MoveTo((short)(d.right + 6), (short)(hdrBox.top + 13));
    DrawString("\pPixel");
    TextFont(bodyFont);
    hline(&hdrBox, HDR_H - 3);                            /* double line, as under a Finder window's header */
    hline(&hdrBox, HDR_H - 1);
}

/* ------------------------------------------------------------------ transcript drawing */
static void centerText(const char *s, short n, short y)    /* centred in the transcript's width */
{
    MoveTo((short)((area.left + area.right - TextWidth(s, 0, n)) / 2), y);
    DrawText(s, 0, n);
}

static short topRow(void) { return (short)(nRows > visLines ? nRows - visLines : 0); }
static short rowY(short i) { return (short)(area.top + ROWS_TOP + (i - topRow()) * lineH + ascent); }

static void drawRow(short i)
{
    Rect r = area;
    Row *w = &rows[i];
    short line = (short)(i - topRow()), y;
    if (line < 0 || line >= visLines) return;
    r.top = (short)(area.top + ROWS_TOP + line * lineH);
    r.bottom = (short)(r.top + lineH);
    EraseRect(&r);
    y = (short)(r.top + ascent);
    if (w->kind == RK_YOU || w->kind == RK_PIXEL) {
        const unsigned char *lab = w->kind == RK_YOU ? "\pYou" : "\pPixel";
        TextFont(0);
        MoveTo((short)(labR - StringWidth(lab)), y);
        DrawString(lab);
        TextFont(bodyFont);
    }
    if (w->kind == RK_NOTE) centerText(w->t, w->len, y);
    else {
        MoveTo(textX, y);
        DrawText(w->t, 0, w->len);
    }
}

static void newRow(short kind)
{
    Row *w;
    if (nRows == MAXROWS) {             /* forget the oldest row */
        BlockMove(rows + 1, rows, (MAXROWS - 1) * sizeof(Row));
        nRows--;
    }
    cur = w = &rows[nRows++];
    w->len = 0;
    w->kind = (unsigned char)kind;
    rowW = 0;
    if (nRows > visLines) {             /* scroll the transcript up one row */
        RgnHandle rgn = NewRgn();
        Rect r = area;
        r.top = (short)(area.top + ROWS_TOP);
        r.bottom = (short)(r.top + visLines * lineH);
        ScrollRect(&r, 0, (short)-lineH, rgn);
        DisposeRgn(rgn);
    }
    drawRow((short)(nRows - 1));
}

/* typewriter output with word wrap; draw = 0 only stores (notes are drawn centred afterwards) */
static void putChar(char c, int draw)
{
    short cw = CharWidth(c), n = cur->len, kind = cur->kind, line = (short)(nRows - 1);
    if (c == ' ' && !n) return;                       /* no leading blanks */
    if (n >= LINELEN || rowW + cw > (kind == RK_NOTE ? area.right - area.left - 2 * MARGIN : textR - textX)) {
        Row *old = cur;
        short sp = n;
        if (kind != RK_NOTE) kind = RK_TEXT;
        if (c == ' ') { newRow(kind); return; }
        while (sp > 0 && old->t[sp - 1] != ' ') sp--;
        if (sp <= 0) sp = n;                          /* one long word: break it */
        n = (short)(n - sp);                          /* the last word (n chars) moves to a new row */
        old->len = (unsigned char)sp;
        if (draw && n) drawRow(line);
        newRow(kind);
        if (nRows - 1 == line) old--;                 /* the history moved up one row */
        BlockMove(old->t + sp, cur->t, n);
        cur->len = (unsigned char)n;
        rowW = TextWidth(cur->t, 0, n);
        line = (short)(nRows - 1);
        if (draw && n) drawRow(line);
    }
    cur->t[n] = c;
    cur->len = (unsigned char)(n + 1);
    if (draw && kind != RK_NOTE) {
        MoveTo((short)(textX + rowW), rowY(line));
        DrawChar(c);
    }
    rowW = (short)(rowW + cw);
}

static void putText(const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++) putChar(s[i], 1);
}

static void say(const char *s)          /* a note from the app, centred once complete (up to 3 rows) */
{
    short i;
    newRow(RK_NOTE);
    while (*s) putChar(*s++, 0);
    for (i = (short)(nRows > 3 ? nRows - 3 : 0); i < nRows; i++) drawRow(i);   /* rows before it look the same */
}

/* ------------------------------------------------------------------ input bar */
static void drawInput(void)
{
    Rect r = inputBox;
    short start = inLen, w = 0, x, avail = (short)(inputBox.right - inputBox.left - 14);
    InsetRect(&r, 1, 1);
    EraseRect(&r);
    while (start > 0) {                                  /* the tail of the line that fits */
        short cw = CharWidth(input[start - 1]);
        if (w + cw > avail) break;
        w = (short)(w + cw);
        start--;
    }
    MoveTo((short)(inputBox.left + 5), (short)(inputBox.bottom - 6));
    DrawText(input, start, inLen - start);
    x = (short)(inputBox.left + 6 + w);                 /* the caret */
    MoveTo(x, (short)(inputBox.top + 3));
    LineTo(x, (short)(inputBox.bottom - 4));
}

static void drawInputBar(void)
{
    EraseRect(&inBar);
    hline(&inBar, 0);
    hline(&inBar, 2);
    FrameRect(&inputBox);
    drawInput();
#ifdef PX_KB
    FrameRoundRect(&kbButton, 16, 16);                  /* the keyboard button */
    TextFont(0);
    MoveTo((short)((kbButton.left + kbButton.right - StringWidth("\pKeys")) / 2), (short)(kbButton.bottom - 6));
    DrawString("\pKeys");
    TextFont(bodyFont);
#endif
}

static void drawAll(void)
{
    short i;
#ifdef PX_KB
    unsigned long t0 = TickCount();
#endif
    SetPort(win);                 /* its font is bodyFont: every function that changes it restores it */
    EraseRect(&win->portRect);
    drawHeader();
    for (i = topRow(); i < nRows; i++) drawRow(i);
    drawInputBar();
#ifdef PX_KB
    if (kbOpen) drawKeys();
#endif
    drawStatus(dbg.state == 3 ? "thinking\311" : dbg.variant ? "B5" : "W4");
#ifdef PX_KB
    dbg.kbTicks[1] = TickCount() - t0;
#endif
}

static void layout(void)
{
    short mb = LMGetMBarHeight(), y;
    FontInfo fi;
    scr = qd.screenBits.bounds;
    SetRect(&hdrBox, scr.left, (short)(scr.top + mb), scr.right, (short)(scr.top + mb + HDR_H));
    y = scr.bottom;
#ifdef PX_KB
    SetRect(&kbArea, scr.left, (short)(y - KB_H), scr.right, y);
    if (kbOpen) y = kbArea.top;
#endif
    SetRect(&inBar, scr.left, (short)(y - INBAR_H), scr.right, y);
    SetRect(&inputBox, (short)(scr.left + MARGIN - 4), (short)(inBar.top + 6), (short)(scr.right - MARGIN + 4),
            (short)(inBar.bottom - 3));
#ifdef PX_KB
    inputBox.right -= KEYS_W + 8;
    SetRect(&kbButton, (short)(inputBox.right + 8), inputBox.top, (short)(scr.right - MARGIN + 4), inputBox.bottom);
#endif
    SetRect(&area, scr.left, hdrBox.bottom, scr.right, inBar.top);
    bodyFont = RealFont(3, 12) ? 3 : 0;         /* Geneva 12, or Chicago where the System has no Geneva 12 */
    TextFont(0);
    TextSize(12);
    labR = (short)(scr.left + MARGIN + StringWidth("\pPixel"));
    textX = (short)(labR + LABGAP);
    textR = (short)(scr.right - MARGIN);
    TextFont(bodyFont);
    GetFontInfo(&fi);
    ascent = fi.ascent;
    lineH = (short)(fi.ascent + fi.descent + fi.leading);
    visLines = (short)((area.bottom - area.top - ROWS_TOP - 2) / lineH);
    if (visLines > MAXROWS) visLines = MAXROWS;
#ifdef PX_KB
    ((Rect *)dbg.geo)[0] = kbArea;
    ((Rect *)dbg.geo)[1] = kbButton;
    ((Rect *)dbg.geo)[2] = inputBox;
    ((Rect *)dbg.geo)[3] = area;
#endif
}

static void centerMsg(const char *a, const char *b)
{
    Rect r = area;
    SetPort(win);
    EraseRect(&r);
    TextFont(0);
    centerText(a, (short)strlen(a), (short)((r.top + r.bottom) / 2 - 10));
    TextFont(bodyFont);
    if (b) centerText(b, (short)strlen(b), (short)((r.top + r.bottom) / 2 + 10));
}

static void progress(int pct)
{
    Rect r, f;
    char s[40];
    int n = 0;
#ifndef PX_NO_B5
    {
        long fm = FreeMem();
        if (!dbg.minFree || fm < dbg.minFree) dbg.minFree = fm;
    }
#endif
    SetPort(win);
    memcpy(s, "Waking Pixel up\311 ", 17);
    n = 17;
    if (pct >= 100) s[n++] = '1';
    if (pct >= 10) s[n++] = (char)('0' + (pct / 10) % 10);
    s[n++] = (char)('0' + pct % 10);
    s[n++] = '%';
    r = area;
    r.top = (short)((area.top + area.bottom) / 2 - 24);
    r.bottom = (short)(r.top + 20);
    EraseRect(&r);
    TextFont(0);
    centerText(s, (short)n, (short)(r.bottom - 5));
    TextFont(bodyFont);
    SetRect(&r, (short)((area.left + area.right) / 2 - 100), (short)((area.top + area.bottom) / 2 + 24),
            (short)((area.left + area.right) / 2 + 100), (short)((area.top + area.bottom) / 2 + 34));
    FrameRect(&r);
    f = r;
    InsetRect(&f, 1, 1);
    f.right = (short)(f.left + (f.right - f.left) * pct / 100);
    PaintRect(&f);
}

/* ------------------------------------------------------------------ model parts and disk swap */
typedef struct { short ref; int part; unsigned long id; long fpos; } Parts;
static Parts parts;
static short bootVol, bootEjected;

static short openPartOn(int part, short vref)
{
    static unsigned char n1[] = "\pPixel Model 1", n2[] = "\pPixel Model 2";
    short ref;
    long cnt = 16;
    unsigned char h[16];
    if (FSOpen(part == 1 ? n1 : n2, vref, &ref) != noErr) return 0;
    if (FSRead(ref, &cnt, h) != noErr || cnt != 16 || h[0] != 'P' || h[1] != 'X' || h[2] != 'P' || h[3] != 'T'
        || h[5] != part) { FSClose(ref); return 0; }
    {
        unsigned long id = ((unsigned long)h[8] << 24) | ((unsigned long)h[9] << 16) | ((unsigned)h[10] << 8) | h[11];
        if (part == 1) parts.id = id;
        else if (id != parts.id) { FSClose(ref); return 0; }
    }
    return ref;
}

static void mountPending(int ticks)    /* mount disks sitting in drives (inserted before launch) */
{
    EventRecord ev;
    unsigned long t0 = TickCount();
    QElemPtr q;
    for (q = LMGetDrvQHdr().qHead; q; q = q->drvQElem.qLink) {
        DrvQEl *d = &q->drvQElem;
        unsigned char inPlace = ((unsigned char *)d)[-3];   /* flags byte 1: disk in place */
        if (inPlace == 1 || inPlace == 2) {
            ParamBlockRec pb;
            memset(&pb, 0, sizeof pb);
            pb.volumeParam.ioVRefNum = d->dQDrive;
            PBMountVol(&pb);                                /* volOnLinErr if already mounted */
        }
    }
    while (TickCount() - t0 < (unsigned long)ticks) {
        SystemTask();
        GetNextEvent(diskMask, &ev);
    }
}

static short findPart(int part)
{
    short ref, i;
    HParamBlockRec pb;
    if (!bootEjected && (ref = openPartOn(part, 0)) != 0) return ref;
    for (i = 1; i < 32; i++) {
        memset(&pb, 0, sizeof pb);
        pb.volumeParam.ioVolIndex = i;
        if (PBHGetVInfoSync(&pb) != noErr) break;
        if (pb.volumeParam.ioVDrvInfo == 0) continue;          /* offline (ejected) volume */
        if ((ref = openPartOn(part, pb.volumeParam.ioVRefNum)) != 0) return ref;
    }
    return 0;
}

static short askForDisk2(void)
{
    EventRecord ev;
    short ref;
    dbg.swaps++;
    if (!bootEjected) { Eject(NULL, bootVol); bootEjected = 1; }   /* boot disk contents are in RAM */
    centerMsg("Insert Pixel Disk 2", "Pixel keeps the rest of its brain there.");
    for (;;) {
        SystemTask();
        if (!GetNextEvent(diskMask | updateMask, &ev)) continue;
        if (ev.what == updateEvt) {
            BeginUpdate((WindowPtr)ev.message);
            EndUpdate((WindowPtr)ev.message);
            centerMsg("Insert Pixel Disk 2", "Pixel keeps the rest of its brain there.");
            continue;
        }
        if (ev.what != diskEvt) continue;
        if (HiWord(ev.message) == noErr && (ref = findPart(2)) != 0) {
            centerMsg("Thank you!", "Pixel is waking up...");
            return ref;
        }
        {   /* wrong or unreadable disk: give it back */
            short drv = LoWord(ev.message);
            if (HiWord(ev.message) == noErr) UnmountVol(NULL, drv);
            Eject(NULL, drv);
            dbg.swaps++;
            centerMsg("That is not Pixel Disk 2.", "Please insert Pixel Disk 2.");
        }
    }
}

static int32_t readModel(void *ctx, void *buf, int32_t n)
{
    char *p = (char *)buf;
    int32_t got = 0;
    (void)ctx;
    while (got < n) {
        long cnt;
        OSErr e;
        if (!parts.ref) {
            if (parts.part >= 2) return got;
            parts.part++;
            parts.ref = findPart(parts.part);
            if (!parts.ref && parts.part == 2) {
                mountPending(120);              /* a second disk may still be waiting to mount */
                parts.ref = findPart(2);
            }
            if (!parts.ref) {
                if (parts.part == 1) return got;
                parts.ref = askForDisk2();
            }
            parts.fpos = 16;                    /* after the part header */
        }
        cnt = n - got;
        if (parts.fpos & 511) {                 /* keep reads aligned to 512-byte file blocks */
            long k = 512 - (parts.fpos & 511);
            if (cnt > k) cnt = k;
        }
        {
            unsigned long t0 = TickCount();
            e = FSRead(parts.ref, &cnt, p + got);
            dbg.readTicks += TickCount() - t0;
        }
        got += cnt;
        parts.fpos += cnt;
        if (e == eofErr || (e == noErr && cnt == 0)) { FSClose(parts.ref); parts.ref = 0; }
        else if (e != noErr) return got;
    }
    return got;
}

static void *allocPtr(int32_t n) { return NewPtr(n); }
static void freePtr(void *p) { DisposePtr((Ptr)p); }
static uint32_t tickClock(void) { return TickCount(); }

/* ------------------------------------------------------------------ test script */
static char script[2048];
static long scriptLen, scriptPos;
static int testMode, forceVariant;

static void loadScript(void)
{
    short ref, i;
    long cnt = sizeof script - 1;
    HParamBlockRec pb;
    if (FSOpen("\pPixel Test", 0, &ref) != noErr) {
        for (i = 1; i < 32; i++) {          /* also the top level of each disk (SE layout) */
            memset(&pb, 0, sizeof pb);
            pb.volumeParam.ioVolIndex = i;
            if (PBHGetVInfoSync(&pb) != noErr) return;
            if (FSOpen("\pPixel Test", pb.volumeParam.ioVRefNum, &ref) == noErr) break;
        }
        if (i == 32) return;
    }
    FSRead(ref, &cnt, script);
    FSClose(ref);
    script[cnt] = 0;
    scriptLen = cnt;
    testMode = 1;
}

static long parseNum(void)
{
    long v = 0;
    while (scriptPos < scriptLen && (script[scriptPos] == ' ' || script[scriptPos] == '\r' || script[scriptPos] == '\n')) scriptPos++;
    while (scriptPos < scriptLen && script[scriptPos] >= '0' && script[scriptPos] <= '9') v = v * 10 + script[scriptPos++] - '0';
    return v;
}

static int nextScriptLine(char *out)
{
    int n = 0;
    while (scriptPos < scriptLen && (script[scriptPos] == '\r' || script[scriptPos] == '\n')) scriptPos++;
    if (scriptPos >= scriptLen) return -1;
    while (scriptPos < scriptLen && script[scriptPos] != '\r' && script[scriptPos] != '\n' && n < 199)
        out[n++] = script[scriptPos++];
    out[n] = 0;
    return n;
}

#ifdef PIXEL_SELFTEST
/* kernel self-test (test build only): 68000 kernels against their C versions on random data */
void c_tab4(const int16_t *, long, int16_t *);
void c_mvw(const int16_t *, const int16_t *, long, long, int16_t *);
void c_tab5(const int16_t *, long, int16_t *);
void c_mvb5(const uint8_t *, const int16_t *, long, long, int16_t *);
void c_pairs(const int16_t *, long, int16_t *);
void c_ptab(const int16_t *, long, int16_t *);
void c_wsum(const uint8_t *, const int16_t *, long, int16_t *);
unsigned long c_maxabs16(const int16_t *, long);
unsigned long c_maxabs32(const int32_t *, long);
void c_apply16(const int16_t *, int16_t *, long, long, long);
void c_addres(int32_t *, const int16_t *, long, long, long);
void c_shr32to16(const int32_t *, int16_t *, long, long);
unsigned long c_normpass(const int32_t *, const int16_t *, int16_t *, int32_t *, long, long, unsigned long *);
unsigned long c_ffnact(const int16_t *, const int16_t *, int32_t *, long, long, long, const int16_t *);
unsigned long c_shrmax(const int32_t *, int16_t *, long, long);
void c_headk(const int16_t *, const int16_t *, int32_t *, long);
void c_mvw32(const int16_t *, const int16_t *, long, int16_t *);
void c_exp5(const uint8_t *, int16_t *, long, uint32_t *);

void c_apply16c(const int16_t *, int16_t *, long, long, long, long);
long c_dot8(const int8_t *, const int16_t *, long);
long c_scores(const int16_t *, const int16_t *, long, long, long, const int16_t *, long, int16_t *);
long c_softp(const int16_t *, long, long, const int16_t *, int16_t *);
unsigned long c_softw(const int16_t *, const int16_t *, long, long, uint32_t *);
void c_rshr(const uint32_t *, int16_t *, long, long);

static long kernelSelfTest(void)
{
    static int16_t x[520], codes[8 * 128], o1[64], o2[64], u[64];
    static uint8_t b[8 * 103], vb[64 * 40];
    int16_t *l1 = (int16_t *)NewPtr(243L * 103 * 2), *l2 = (int16_t *)NewPtr(243L * 103 * 2);
    long bad = 0, i, rep;
    uint32_t s = 12345;
    if (!l1 || !l2) return -2;
#define RND (s ^= s << 13, s ^= s >> 17, s ^= s << 5, s)
    for (rep = 0; rep < 4; rep++) {
        for (i = 0; i < 520; i++) x[i] = (int16_t)RND;
        for (i = 0; i < 8 * 128; i++) codes[i] = (int16_t)(((RND % 81) + 81 * (i % 128)) * 2 - 32768);
        for (i = 0; i < 8 * 103; i++) b[i] = (uint8_t)(RND % 243);
        k_tab4(x, 128, l1); c_tab4(x, 128, l2);
        for (i = 0; i < 81 * 128; i++) bad += l1[i] != l2[i];
        k_mvw(codes, l1 + 16384, 16, 64, o1); c_mvw(codes, l2 + 16384, 16, 64, o2);
        for (i = 0; i < 16; i++) bad += o1[i] != o2[i];
        k_mvw40(codes, l1 + 16384, 25, o1); c_mvw(codes, l2 + 16384, 25, 40, o2);
        for (i = 0; i < 25; i++) bad += o1[i] != o2[i];
        k_mvw32(codes, l1 + 16384, 32, o1); c_mvw(codes, l2 + 16384, 32, 32, o2);
        for (i = 0; i < 32; i++) bad += o1[i] != o2[i];
        {
            uint32_t f1[2] = {1, 2}, f2[2] = {1, 2};
            static int16_t e1[256], e2[256];
            k_exp5(b, e1, 8, f1); c_exp5(b, e2, 8, f2);
            for (i = 0; i < 256; i++) bad += e1[i] != e2[i];
            bad += f1[0] != f2[0] || f1[1] != f2[1];
        }
        k_tab5(x, 103, l1); c_tab5(x, 103, l2);
        for (i = 0; i < 243 * 103; i++) bad += l1[i] != l2[i];
        k_mvb5(b, l1, 8, 103, o1); c_mvb5(b, l2, 8, 103, o2);
        for (i = 0; i < 8; i++) bad += o1[i] != o2[i];
        k_pairs(x, 40, l1); c_pairs(x, 40, l2);
        for (i = 0; i < 40 * 256; i++) bad += l1[i] != l2[i];
        for (i = 0; i < 64; i++) u[i] = (int16_t)RND;
        for (i = 0; i < 64 * 40; i++) vb[i] = (uint8_t)(2 * (RND & 15));
        k_ptab(u, 64, l1); c_ptab(u, 64, l2);
        for (i = 0; i < 64 * 16; i++) bad += l1[i] != l2[i];
        k_wsum(vb, l1, 64, o1); c_wsum(vb, l1, 64, o2);
        for (i = 0; i < 40; i++) bad += o1[i] != o2[i];
    }
    /* scalar loops: all shift amounts, edge values (-32768, INT32_MIN, saturation) */
    {
        static int32_t r1[512], r2[512], a1[512], a2[512];
        static int16_t t1[512], t2[512];
        unsigned long m1, m2;
        long k;
        for (rep = 0; rep < 3; rep++) {
            for (i = 0; i < 512; i++) {
                x[i] = (int16_t)RND;
                r1[i] = r2[i] = (int32_t)RND >> (RND & 15);
            }
            x[5] = -32768;
            r1[7] = r2[7] = (int32_t)0x80000000UL;
            r1[9] = r2[9] = 0x7FFFFFF0L;
            bad += k_maxabs16(x, 512) != c_maxabs16(x, 512);
            bad += k_maxabs32(r1, 512) != c_maxabs32(r1, 512);
            for (k = 1; k <= 30; k++) {
                long mm = 16384 + (long)(RND & 16383);
                k_apply16(x, t1, 512, mm, k); c_apply16(x, t2, 512, mm, k);
                if (k >= 16) for (i = 0; i < 512; i++) bad += t1[i] != t2[i];
                else for (i = 0; i < 512; i++) bad += (int16_t)t1[i] != (int16_t)t2[i];
                k_addres(r1, x, 160, mm, k); c_addres(r2, x, 160, mm, k);
                for (i = 0; i < 160; i++) bad += r1[i] != r2[i];
                m1 = k_ffnact(x, x + 7, a1, 512, mm, k, px_silu + 512);
                m2 = c_ffnact(x, x + 7, a2, 512, mm, k, px_silu + 512);
                bad += m1 != m2;
                for (i = 0; i < 512; i++) bad += a1[i] != a2[i];
            }
            for (k = 0; k <= 16; k++) {
                k_shr32to16(r1, t1, 512, k); c_shr32to16(r1, t2, 512, k);
                for (i = 0; i < 512; i++) bad += t1[i] != t2[i];
                bad += k_shrmax(r1, t1, 512, k) != c_shrmax(r1, t2, 512, k);
                for (i = 0; i < 512; i++) bad += t1[i] != t2[i];
            }
            k_headk(x, x + 100, r1, 200); c_headk(x, x + 100, r2, 200);
            for (i = 0; i < 200; i++) bad += r1[i] != r2[i];
            for (k = 1; k <= 30; k++) {      /* clamp, router dot, attention scaling/softmax */
                static int16_t sk[128], z1[64], z2[64], bias[64];
                static uint32_t w1[64], w2[64];
                long mm = 16384 + (long)(RND & 16383), e0 = -(long)(RND % 40);
                k_apply16c(x, t1, 160, mm, k, 127); c_apply16c(x, t2, 160, mm, k, 127);
                for (i = 0; i < 160; i++) dbg.kbad[0] += t1[i] != t2[i];
                dbg.kbad[1] += k_dot8((const int8_t *)x, x + 3, 160) != c_dot8((const int8_t *)x, x + 3, 160);
                for (i = 0; i < 64; i++) {
                    sk[2 * i] = (int16_t)((RND & 7) ? 16384 + (RND & 16383) : 0);
                    sk[2 * i + 1] = (int16_t)(e0 + (long)(RND % 24) - 12);
                    bias[i] = (int16_t)(RND % 1100);
                }
                dbg.kbad[2] += k_scores(x, sk, 64, mm, e0 - 15 + k, bias, 100 + k, z1)
                    != c_scores(x, sk, 64, mm, e0 - 15 + k, bias, 100 + k, z2);
                for (i = 0; i < 64; i++) dbg.kbad[2] += z1[i] != z2[i];
                {
                    long zm = 32767 - (long)(RND & 255);
                    for (i = 0; i < 64; i++) if (z1[i] > zm) z1[i] = (int16_t)zm;
                    dbg.kbad[3] += k_softp(z1, 64, zm, px_exp12, t1) != c_softp(z1, 64, zm, px_exp12, t2);
                }
                for (i = 0; i < 64; i++) dbg.kbad[3] += t1[i] != t2[i];
                dbg.kbad[4] += k_softw(t1, sk, 64, e0 + 12, w1) != c_softw(t1, sk, 64, e0 + 12, w2);
                for (i = 0; i < 64; i++) dbg.kbad[4] += w1[i] != w2[i];
                k_rshr(w1, t1, 64, k % 19); c_rshr(w1, t2, 64, k % 19);
                for (i = 0; i < 64; i++) dbg.kbad[5] += t1[i] != t2[i];
            }
            for (k = -14; k <= 17; k++) {
                unsigned long s1, s2;
                s1 = k_normpass(r1, x, t1, a1, 160, k, &m1);
                s2 = c_normpass(r1, x, t2, a2, 160, k, &m2);
                bad += s1 != s2 || m1 != m2;
                for (i = 0; i < 160; i++) bad += t1[i] != t2[i] || a1[i] != a2[i];
            }
        }
    }
    DisposePtr((Ptr)l2);
    DisposePtr((Ptr)l1);
    for (i = 0; i < 6; i++) bad += dbg.kbad[i];
    dbg.kbad[6] = bad;
    return bad;
}
#endif

/* ------------------------------------------------------------------ chat */
static void logTok(int t)
{
    if (dbg.ntok < LOGMAX) tokLog[dbg.ntok++] = (int16_t)t;
}

static void pollTyping(void);

static void step(int t, int logits)
{
    unsigned long t0 = TickCount();
    px_step(&M, t, logits);
    if (logits) { dbg.genTicks += TickCount() - t0; dbg.genSteps++; }
    else { dbg.ingTicks += TickCount() - t0; dbg.ingSteps++; }
    if (!testMode) pollTyping();
}

/* ------------------------------------------------------------------ menus: Apple, File */
static MenuHandle appleM;
static int busyNow, abortReply;

static void newChat(void)
{
    px_reset(&M);
    nRows = 0;
    drawAll();
    say("New chat. Pixel forgot everything.");
}

#ifdef PX_KB
static void kbToggle(void)        /* the transcript keeps its rows: it shows the newest that fit */
{
    kbOpen = (short)!kbOpen;
    kbCaps = 0;
    layout();
    drawAll();
}

static int doKey(EventRecord *ev, int busy);

/* a click on the on-screen keyboard: the key goes through doKey like a typed one */
static int kbClick(Point pt)
{
    short k;
    Rect r;
    EventRecord e;
    unsigned long t;
    for (k = 0; k < 47; k++) {
        char c = kbKey(k, &r);
        if (!PtInRect(pt, &r)) continue;
        InsetRect(&r, 1, 1);
        InvertRoundRect(&r, 6, 6);
        t = TickCount();
        while (TickCount() - t < 6) { }
        InvertRoundRect(&r, 6, 6);
        if (c == 1) { kbCaps = (short)!kbCaps; drawKeys(); return 0; }
        if (c == 2) { kbToggle(); return 0; }
        e.message = (unsigned char)(kbCaps && c >= 'a' && c <= 'z' ? c - 32 : c);
        e.modifiers = 0;
        return doKey(&e, busyNow);
    }
    return 0;
}
#endif /* PX_KB */

static void doMenu(long mr)
{
    short menu = (short)(mr >> 16), item = (short)mr;
    if (menu == 128) {
        if (item == 1) Alert(130, NULL);
        else if (item > 2) {
            Str255 name;
            GetMenuItemText(appleM, item, name);
            OpenDeskAcc(name);
        }
    } else if (menu == 129) {
        if (item == 1) { if (busyNow) abortReply = 1; else newChat(); }
        else if (item == 2) ExitToShell();
    }
#ifdef PX_KB
    else if (menu == 130) kbToggle();
#endif
    HiliteMenu(0);
}

static int doMouse(EventRecord *ev)    /* returns 1 if the on-screen Send key was clicked (idle) */
{
    WindowPtr w;
    short part = FindWindow(ev->where, &w);
    if (part == inMenuBar) doMenu(MenuSelect(ev->where));
    else if (part == inSysWindow) SystemClick(ev, w);
#ifdef PX_KB
    else if (part == inContent) {
        if (PtInRect(ev->where, &kbButton)) kbToggle();
        else if (kbOpen && PtInRect(ev->where, &kbArea)) return kbClick(ev->where);
    }
#endif
    return 0;
}

/* one keyboard event: edit the input line; returns 1 for Return (only when not busy) */
static int doKey(EventRecord *ev, int busy)
{
    char c = (char)(ev->message & charCodeMask);
    if (ev->modifiers & cmdKey) {      /* menu shortcuts: N new chat, Q quit */
        doMenu(MenuKey(c));
        return 0;
    }
    if (c == '\r' || c == 3) return !busy && inLen;
    if (c == 8) { if (inLen) inLen--; }
    else if (c >= 32 && c <= 126 && inLen < (short)sizeof input - 1) input[inLen++] = c;
    else return 0;
    drawInput();
    return 0;
}

static void pollTyping(void)      /* while Pixel thinks: keys, menus and redraws between tokens */
{
    EventRecord ev;
    /* GetNextEvent only when something waits: under System 7 each call lets the Finder run (about
       4 ticks per token on the SE); OSEventAvail and the update region don't switch processes */
    if (!OSEventAvail(keyDownMask | autoKeyMask | mDownMask, &ev) && EmptyRgn(((WindowPeek)win)->updateRgn)) return;
    while (GetNextEvent(keyDownMask | autoKeyMask | mDownMask | updateMask, &ev)) {
        if (ev.what == mouseDown) doMouse(&ev);
        else if (ev.what == updateEvt) {
            BeginUpdate((WindowPtr)ev.message);
            drawAll();
            EndUpdate((WindowPtr)ev.message);
        } else doKey(&ev, 1);
    }
}

static void handleLine(const char *text, int n)
{
    static char buf[260];
    static int16_t ids[300];
    int k, i, cnt;
    if (n == 5 && !memcmp(text, "reset", 5)) {
        px_reset(&M);
        say("(Pixel forgot everything.)");
        logTok(-1);
        return;
    }
    newRow(RK_YOU);
    putText(text, n);
    memcpy(buf, "<user> ", 7);
    memcpy(buf + 7, text, n);
    memcpy(buf + 7 + n, "<pixel>", 7);
    cnt = px_encode(&M, buf, n + 14, ids, 300);
    dbg.state = 3;                 /* text is encoded: the input line is free for typing ahead */
    SetCursor(&hourglass);
    drawStatus("thinking\311");
    busyNow = 1;
    for (i = 0; i < cnt && !abortReply; i++) step(ids[i], i == cnt - 1);
    newRow(RK_PIXEL);
    for (k = 0; !abortReply; k++) {
        int t;
        {
            unsigned long t0 = TickCount();
            t = px_sample(&M, temp256, topk);
            px_prof[PP_SAMPLE] += TickCount() - t0;
        }
        logTok(t);
        if (t < 4) { step(PX_END, 0); break; }     /* <end> (or any marker): the turn is over */
        {
            char s[48];
            int m = px_tokstr(&M, t, s, 48);
            putText(s, m);                          /* a leading blank is dropped at the row start */
        }
        if (k + 1 == maxNew) { step(t, 0); step(PX_END, 0); break; }
        step(t, 1);
    }
    busyNow = 0;
    if (abortReply) { abortReply = 0; dbg.state = 2; newChat(); }
    dbg.turns++;
    for (i = 0; i < PP_N; i++) dbg.prof[i] = px_prof[i];
    InitCursor();
    drawStatus(dbg.variant ? "B5" : "W4");
    dbg.state = 2;
}

int main(void)
{
    EventRecord ev;
    Rect r;
    long need;
    int err, i;

    dbg.magic = MAGIC;
    dbg.version = 1;
    dbg.notMagic = ~MAGIC;
    dbg.kself = -1;
    dbg.logAddr = (unsigned long)tokLog;

    SetApplLimit((Ptr)((long)LMGetApplLimit() - 32768L));   /* 32 KB more stack */
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
    FlushEvents(everyEvent - diskMask, 0);   /* keep disk-inserted events: they mount disk 2 */

    appleM = GetMenu(128);
    AppendResMenu(appleM, 'DRVR');              /* desk accessories, if the System has any */
    InsertMenu(appleM, 0);
    InsertMenu(GetMenu(129), 0);
#ifdef PX_KB
    InsertMenu(GetMenu(130), 0);                /* Edit: Keyboard... (kb.r, PixelSE only) */
#endif
    DrawMenuBar();
    r = qd.screenBits.bounds;     /* the whole screen; the menu bar stays on top of it */
    win = NewWindow(NULL, &r, "\p", true, plainDBox, (WindowPtr)-1, false, 0);
    SetPort(win);
    layout();
    drawAll();
    ValidRect(&win->portRect);    /* drawn: drop the update pending since NewWindow */
    GetVol(NULL, &bootVol);
    loadScript();
    if (testMode) {      /* script header: seed temp256 topk max_new force_variant(0 auto, 1 W4, 2 B5) */
        dbg.seed = (unsigned long)parseNum();
        temp256 = (int)parseNum();
        topk = (int)parseNum();
        maxNew = (int)parseNum();
        forceVariant = (int)parseNum();
    }

    dbg.maxBlock = MaxBlock();
    need = px_bytes_needed(PX_W4) + 160000L;
    dbg.variant = dbg.maxBlock >= need ? PX_W4 : PX_B5;
    if (forceVariant) dbg.variant = forceVariant - 1;
#ifndef PX_NO_B5
    drawStatus(dbg.variant ? "B5" : "W4");    /* the window was drawn before the choice */
#endif
#ifdef PX_NO_B5
    if (dbg.variant != PX_W4) {        /* the Plus build has only the fast (W4) kernels */
        centerMsg("Pixel needs a Mac with 4 MB of memory.", "(Pixel SE runs in less memory.)");
        dbg.state = -1;
        for (;;) SystemTask();
    }
#endif
    if (dbg.variant == PX_B5 && dbg.maxBlock < px_bytes_needed(PX_B5) + 60000L) {
        centerMsg("Pixel needs more memory.", "Please run Pixel on a Mac with 2.5 MB free.");
        dbg.state = -1;
        for (;;) SystemTask();
    }
    centerMsg("", dbg.variant ? "(compact memory mode)" : "(fast mode)");
    progress(0);
    dbg.state = 1;
    SetCursor(&hourglass);
#ifdef PIXEL_SELFTEST
    px_clock = tickClock;
#endif
    dbg.loadTicks = TickCount();
    err = px_load(&M, (int)dbg.variant, readModel, 0, allocPtr, freePtr, progress);
    dbg.loadTicks = TickCount() - dbg.loadTicks;
    dbg.loadErr = err;
    for (i = 0; i < PL_N; i++) dbg.lprof[i] = px_lprof[i];
    for (i = 0; i < PP_N; i++) px_prof[i] = 0;
    InitCursor();
    if (err) {
        centerMsg("Pixel could not load its brain.",
                  err == 2 ? "Not enough memory." :
                  err == 15 ? "A model file is missing or could not be read." :
                  err >= 14 ? "The model disks are damaged. Please copy them again." :
                  "The model files are from a different version.");
        dbg.state = -2;
        for (;;) SystemTask();
    }
    dbg.freeAfter = FreeMem();
#ifdef PIXEL_SELFTEST
    dbg.selftest = 1;
    if (testMode) dbg.kself = kernelSelfTest();
    px_clock = tickClock;
#endif
    if (!testMode) dbg.seed = TickCount() * 2654435761UL;
    px_seed(&M, (uint32_t)dbg.seed);
    FlushEvents(keyDownMask | autoKeyMask | mDownMask, 0);   /* keys pressed while loading */
#ifdef PX_KB
    kbOpen = 1;                 /* no keyboard on the lab's SE: start with the on-screen one open */
    layout();
#endif
    drawAll();
    ValidRect(&win->portRect);  /* drop any pending update: its full redraw would race the first clicks */
    say("Pixel is here! Type something and press Return.");
    dbg.state = 2;

    for (;;) {
        SystemTask();
        if (testMode && dbg.state == 2) {
            char line[200];
            int n = nextScriptLine(line);
            if (n < 0) { dbg.state = 4; testMode = 0; continue; }
            handleLine(line, n);
            continue;
        }
        if (!GetNextEvent(everyEvent, &ev)) continue;
        switch (ev.what) {
        case keyDown:
        case autoKey:
            if (doKey(&ev, 0)) {
                int n = inLen;
                inLen = 0;
                drawInput();
                handleLine(input, n);
            }
            break;
        case updateEvt:
            BeginUpdate((WindowPtr)ev.message);
            drawAll();
            EndUpdate((WindowPtr)ev.message);
            break;
        case mouseDown:
#ifdef PX_KB
            if (doMouse(&ev)) {         /* the on-screen Send key */
                int n = inLen;
                inLen = 0;
                drawInput();
                handleLine(input, n);
            }
#else
            doMouse(&ev);
#endif
            break;
        default:
            break;
        }
    }
    return 0;
}

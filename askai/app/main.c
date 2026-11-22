/*
 * Ask AI: chat with modern AI models from a Macintosh Plus or SE (System 6 or 7), through a Hayes
 * Wi-Fi modem on the modem port and the Ask AI relay (askai/relay), which speaks HTTPS to the model
 * APIs and keeps the keys.
 *
 * One borderless window covers the screen (qd.screenBits.bounds, so Narrow Screen is respected).
 * Under the menu bar, top to bottom:
 *   header   icon, "Ask AI", the status line at the right (state, model, bytes/s); double line
 *   area     the transcript: "You" / model labels in a column, word-wrapped text, a scroll bar
 *   inBar    double line, then the input line
 * Replies stream in word by word; you can type the next message meanwhile (Return queues it).
 * Settings live in "Ask AI Prefs" in the System Folder (layout: link.h; tools/prefs.py writes it too).
 *
 * A debug block (magic 'ASK!') lets MAME Lua scripts follow the app (askai/tests/chat.lua).
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

#include <SegLoad.h>
#include <string.h>
#include "serial.h"
#include "link.h"

#define MAGIC 0x41534B21UL     /* 'ASK!' */

struct Dbg {
    unsigned long magic, version, notMagic;   /*  0,  4,  8 */
    long state;          /* 12: link state (link.h) */
    long rxBytes;        /* 16 */
    long txBytes;        /* 20: bytes of messages sent (ASK lines) */
    long replies;        /* 24: replies finished (END) */
    long errors;         /* 28: error notes shown */
    long lastErr;        /* 32: E_* of the last error */
    long rows;           /* 36: transcript rows */
    long nModels;        /* 40 */
    long replyBytes;     /* 44: bytes of the last reply */
    long bps;            /* 48: its bytes per second */
    long serErrors;      /* 52: serial overrun/framing errors */
    long sent;           /* 56: messages sent */
    long scrollTop;      /* 60 */
    long freeMem;        /* 64 */
    char note[64];       /* 68: the last note */
    char model[32];      /* 132: current model id */
    char lastText[64];   /* 164: the end of the last reply's text */
};
static struct Dbg dbg;

static WindowPtr win;
static MenuHandle appleM, fileM, editM, connM, modelM;

/* ------------------------------------------------------------------ layout */
#define HDR_H 21
#define INBAR_H 29
#define MARGIN 10
#define LABGAP 8
#define ROWS_TOP 4
#define SB_W 16
static Rect scr, hdrBox, area, inBar, inputBox, sbRect;
static short lineH = 16, ascent = 12, visLines, bodyFont = 3;
static short labR, textX, textR;
static short cw[256];                   /* character widths in the body font */
static ControlHandle sbar;

static void hline(const Rect *r, short dy)
{
    MoveTo(r->left, (short)(r->top + dy));
    LineTo((short)(r->right - 1), (short)(r->top + dy));
}

/* ------------------------------------------------------------------ transcript rows */
#define MAXROWS 200
#define LINELEN 120
enum { RK_TEXT, RK_SOFT, RK_YOU, RK_AI, RK_NOTE };
typedef struct { unsigned char len, kind; char lab[10]; char t[LINELEN]; } Row;
static Row rows[MAXROWS];
static short nRows, scrollTop, curW;
static short drawFrom, drawX;           /* the last row's text from drawFrom on is not on screen yet */
static short inReply, needRow, termRow;

static char input[400];
static short inLen;
static char queued[400];
static short queuedLen;
static long rxTotal, lastBps = -1;
static unsigned long lastStatusTick;
static short prefsDirty, prefsSaveFailed, quitting;

static short maxTop(void) { return (short)(nRows > visLines ? nRows - visLines : 0); }
static short rowTop(short i) { return (short)(area.top + ROWS_TOP + (i - scrollTop) * lineH); }
static int rowVisible(short i) { return i >= scrollTop && i < scrollTop + visLines; }

static short textWidth(const char *s, short n)
{
    short w = 0;
    while (n--) w = (short)(w + cw[(unsigned char)*s++]);
    return w;
}

static void drawLabel(const char *lab, short y)
{
    unsigned char p[12];
    short n = (short)strlen(lab);
    if (n > 9) n = 9;
    TextFont(0);
    p[0] = (unsigned char)n;
    memcpy(p + 1, lab, n);
    while (p[0] > 1 && StringWidth(p) > labR - scr.left - MARGIN + 6) p[0]--;
    MoveTo((short)(labR - StringWidth(p)), y);
    DrawString(p);
    TextFont(bodyFont);
}

static void drawRow(short i)
{
    Rect r;
    Row *w = &rows[i];
    short y;
    if (!rowVisible(i)) return;
    SetRect(&r, area.left, rowTop(i), sbRect.left, (short)(rowTop(i) + lineH));
    EraseRect(&r);
    y = (short)(r.top + ascent);
    if (w->kind == RK_YOU || w->kind == RK_AI) drawLabel(w->lab, y);
    if (w->kind == RK_NOTE) {
        MoveTo((short)((area.left + sbRect.left - textWidth(w->t, w->len)) / 2), y);
        DrawText(w->t, 0, w->len);
    } else {
        MoveTo(textX, y);
        DrawText(w->t, 0, w->len);
    }
    if (i == nRows - 1) { drawFrom = w->len; drawX = (short)(textX + curW); }
}

static void drawVisible(void)
{
    short i;
    Rect r = area;
    r.right = sbRect.left;
    EraseRect(&r);
    for (i = scrollTop; i < nRows && i < scrollTop + visLines; i++) drawRow(i);
}

static void setScrollBar(void)
{
    if (!sbar) return;
    SetControlMaximum(sbar, maxTop());
    SetControlValue(sbar, scrollTop);
    HiliteControl(sbar, maxTop() > 0 ? 0 : 255);
}

static void scrollTo(short top)
{
    short d;
    if (top > maxTop()) top = maxTop();
    if (top < 0) top = 0;
    d = (short)(top - scrollTop);
    if (!d) return;
    if (d > -visLines && d < visLines) {
        RgnHandle rgn = NewRgn();
        Rect r;
        short i;
        SetRect(&r, area.left, (short)(area.top + ROWS_TOP), sbRect.left, (short)(area.top + ROWS_TOP + visLines * lineH));
        ScrollRect(&r, 0, (short)(-d * lineH), rgn);
        DisposeRgn(rgn);
        scrollTop = top;
        if (d > 0) for (i = (short)(top + visLines - d); i < top + visLines && i < nRows; i++) drawRow(i);
        else for (i = top; i < top - d; i++) drawRow(i);
    } else {
        scrollTop = top;
        drawVisible();
    }
    setScrollBar();
    dbg.scrollTop = scrollTop;
}

static void flushDraw(void)
{
    Row *w;
    if (!nRows) return;
    w = &rows[nRows - 1];
    if (drawFrom < w->len && rowVisible((short)(nRows - 1)) && w->kind != RK_NOTE) {
        MoveTo(drawX, (short)(rowTop((short)(nRows - 1)) + ascent));
        DrawText(w->t, drawFrom, (short)(w->len - drawFrom));
    }
    drawFrom = w->len;
    drawX = (short)(textX + curW);
}

static void newRow(short kind, const char *lab)
{
    Row *w;
    int follow = scrollTop >= maxTop(), shifted = 0;
    flushDraw();
    if (nRows == MAXROWS) {                     /* forget the oldest row */
        BlockMove(rows + 1, rows, (MAXROWS - 1) * sizeof(Row));
        nRows--;
        if (scrollTop > 0) scrollTop--;
        else shifted = 1;                       /* the rows on screen moved up under us */
    }
    w = &rows[nRows++];
    w->len = 0;
    w->kind = (unsigned char)kind;
    strncpy(w->lab, lab ? lab : "", sizeof w->lab - 1);
    w->lab[sizeof w->lab - 1] = 0;
    curW = 0;
    drawFrom = 0;
    drawX = textX;
    if (follow && nRows > visLines) {
        if (scrollTop + visLines == nRows - 1) {     /* one row past the bottom: scroll by one */
            RgnHandle rgn = NewRgn();
            Rect r;
            SetRect(&r, area.left, (short)(area.top + ROWS_TOP), sbRect.left,
                    (short)(area.top + ROWS_TOP + visLines * lineH));
            ScrollRect(&r, 0, (short)-lineH, rgn);
            DisposeRgn(rgn);
            scrollTop = maxTop();
        } else {
            scrollTop = maxTop();
            drawVisible();
        }
    } else if (shifted) drawVisible();
    drawRow((short)(nRows - 1));
    setScrollBar();
    dbg.rows = nRows;
    dbg.scrollTop = scrollTop;
}

/* append text to the last row, wrapping at word boundaries */
static void putText(const char *s, short n)
{
    short i;
    for (i = 0; i < n; i++) {
        char c = s[i];
        Row *w = &rows[nRows - 1];
        short ww = cw[(unsigned char)c];
        if ((unsigned char)c < 32) continue;
        if (c == ' ' && w->len == 0 && w->kind == RK_SOFT) continue;   /* no blank at a wrapped row's start */
        if (w->len >= LINELEN || curW + ww > textR - textX) {
            char tmp[LINELEN];
            short sp = w->len, m;
            if (c == ' ') { newRow(RK_SOFT, NULL); continue; }
            while (sp > 0 && w->t[sp - 1] != ' ') sp--;
            if (sp <= 0) sp = w->len;                /* one long word: break it */
            m = (short)(w->len - sp);
            BlockMove(w->t + sp, tmp, m);
            if (m) {                                 /* the partial word moves to the next row */
                w->len = (unsigned char)sp;
                curW = textWidth(w->t, sp);
                if (drawFrom > sp) drawRow((short)(nRows - 1));   /* part of it was drawn: redraw */
                else flushDraw();
            }
            newRow(RK_SOFT, NULL);
            w = &rows[nRows - 1];
            BlockMove(tmp, w->t, m);
            w->len = (unsigned char)m;
            curW = textWidth(tmp, m);
            drawFrom = 0;
            drawX = textX;
        }
        w->t[w->len++] = c;
        curW = (short)(curW + ww);
    }
    flushDraw();
}

static void note(const char *s)        /* a centred note, wrapped over as many rows as it needs */
{
    short avail = (short)(sbRect.left - area.left - 2 * MARGIN);
    flushDraw();
    while (*s) {
        short n = 0, w = 0, lastSp = -1;
        Row *r;
        while (s[n] && n < LINELEN) {
            short cwid = cw[(unsigned char)s[n]];
            if (w + cwid > avail) break;
            if (s[n] == ' ') lastSp = n;
            w = (short)(w + cwid);
            n++;
        }
        if (s[n] && lastSp > 0) n = lastSp;
        newRow(RK_NOTE, NULL);
        r = &rows[nRows - 1];
        memcpy(r->t, s, n);
        r->len = (unsigned char)n;
        drawRow((short)(nRows - 1));
        s += n;
        while (*s == ' ') s++;
    }
    needRow = 1;
}

static void notec(const char *s)
{
    short n = (short)strlen(s);
    if (n > 63) n = 63;
    memcpy(dbg.note, s, n);
    dbg.note[n] = 0;
    note(s);
}

static void blankBeforeTurn(void)
{
    if (nRows && !(rows[nRows - 1].len == 0 && rows[nRows - 1].kind == RK_TEXT)) newRow(RK_TEXT, NULL);
}

/* ------------------------------------------------------------------ header, status, input */
static const unsigned short icon16[16] = {     /* a small Mac with a speech bubble */
    0x0000, 0x7FC0, 0x4040, 0x5F40, 0x5143, 0x514F, 0x5F7E, 0x407C,
    0x4040, 0x4E40, 0x4040, 0x7FC0, 0x4040, 0x7FC0, 0x0000, 0x0000};

static void drawStatusText(const char *s)
{
    Rect r = hdrBox;
    short n = (short)strlen(s);
    r.left = (short)(hdrBox.left + 90);
    r.bottom = (short)(r.top + HDR_H - 3);
    EraseRect(&r);
    TextFont(3);
    TextSize(9);
    MoveTo((short)(hdrBox.right - MARGIN - TextWidth(s, 0, n)), (short)(hdrBox.top + 12));
    DrawText(s, 0, n);
    TextFont(bodyFont);
    TextSize(12);
}

static void numStr(long v, char *out)
{
    char t[12];
    short n = 0, k = 0;
    if (v < 0) v = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) {
        out[k++] = t[--n];
        if (n && n % 3 == 0) out[k++] = ',';
    }
    out[k] = 0;
}

static void drawStatus(void)
{
    char s[120], num[16];
    GrafPtr old;
    GetPort(&old);
    SetPort(win);
    s[0] = 0;
    if (linkState == L_READY || linkState == L_REPLY) {
        if (curModel >= 0) strcat(s, models[curModel].label);
        else strcat(s, "no model");
        if (linkState == L_REPLY) {
            long t = (long)(TickCount() - replyStart);
            strcat(s, "  \245  receiving");
            if (t > 30 && replyBytes > 0) {
                numStr(replyBytes * 60 / t, num);
                strcat(s, "  ");
                strcat(s, num);
                strcat(s, " B/s");
            }
        } else if (lastBps >= 0) {
            numStr(lastBps, num);
            strcat(s, "  \245  ");
            strcat(s, num);
            strcat(s, " B/s");
        }
    } else strcat(s, link_stateName());
    if (queuedLen) strcat(s, "  \245  1 waiting");
    drawStatusText(s);
    lastStatusTick = TickCount();
    SetPort(old);
}

static void drawHeader(void)
{
    BitMap bm;
    Rect d;
    bm.baseAddr = (Ptr)icon16;
    bm.rowBytes = 2;
    SetRect(&bm.bounds, 0, 0, 16, 16);
    SetRect(&d, (short)(hdrBox.left + MARGIN - 2), (short)(hdrBox.top + 1), (short)(hdrBox.left + MARGIN + 14),
            (short)(hdrBox.top + 17));
    CopyBits(&bm, &win->portBits, &bm.bounds, &d, srcCopy, NULL);
    TextFont(0);
    MoveTo((short)(d.right + 6), (short)(hdrBox.top + 13));
    DrawString("\pAsk AI");
    TextFont(bodyFont);
    hline(&hdrBox, HDR_H - 3);
    hline(&hdrBox, HDR_H - 1);
}

static void drawInput(void)
{
    Rect r = inputBox;
    short start = inLen, w = 0, x, avail = (short)(inputBox.right - inputBox.left - 14);
    InsetRect(&r, 1, 1);
    EraseRect(&r);
    while (start > 0) {
        short c = cw[(unsigned char)input[start - 1]];
        if (w + c > avail) break;
        w = (short)(w + c);
        start--;
    }
    MoveTo((short)(inputBox.left + 5), (short)(inputBox.bottom - 6));
    DrawText(input, start, (short)(inLen - start));
    x = (short)(inputBox.left + 6 + w);
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
}

static void drawAll(void)
{
    SetPort(win);
    EraseRect(&win->portRect);
    drawHeader();
    drawVisible();
    if (sbar) Draw1Control(sbar);
    drawInputBar();
    drawStatus();
}

static void layout(void)
{
    short mb = LMGetMBarHeight(), i;
    FontInfo fi;
    scr = qd.screenBits.bounds;
    SetRect(&hdrBox, scr.left, (short)(scr.top + mb), scr.right, (short)(scr.top + mb + HDR_H));
    SetRect(&inBar, scr.left, (short)(scr.bottom - INBAR_H), scr.right, scr.bottom);
    SetRect(&inputBox, (short)(scr.left + MARGIN - 4), (short)(inBar.top + 6), (short)(scr.right - MARGIN + 4),
            (short)(inBar.bottom - 3));
    SetRect(&area, scr.left, hdrBox.bottom, scr.right, inBar.top);
    SetRect(&sbRect, (short)(scr.right - SB_W), (short)(area.top - 1), (short)(scr.right + 1), (short)(area.bottom + 1));
    bodyFont = RealFont(3, 12) ? 3 : 0;          /* Geneva 12, or Chicago */
    TextFont(0);
    TextSize(12);
    labR = (short)(scr.left + MARGIN + StringWidth("\pGemini"));
    textX = (short)(labR + LABGAP);
    textR = (short)(sbRect.left - 6);
    TextFont(bodyFont);
    GetFontInfo(&fi);
    ascent = fi.ascent;
    lineH = (short)(fi.ascent + fi.descent + fi.leading);
    visLines = (short)((area.bottom - area.top - ROWS_TOP - 2) / lineH);
    for (i = 0; i < 256; i++) cw[i] = CharWidth((char)i);
}

/* ------------------------------------------------------------------ prefs */
static const char *DEFAULT_SYSTEM = "Answer in short plain text for a 1986 Mac with a 512x342 black-and-white "
                                    "screen: no tables, no emoji, short paragraphs, under 150 words unless I ask "
                                    "for more.";

static void pset(unsigned char *p, const char *s, short max)
{
    short n = (short)strlen(s);
    if (n > max) n = max;
    p[0] = (unsigned char)n;
    memcpy(p + 1, s, n);
}

static void defaultPrefs(void)
{
    memset(&prefs, 0, sizeof prefs);
    prefs.magic = PREFS_MAGIC;
    prefs.version = 1;
    prefs.baud = 3;
    prefs.flow = FLOW_NONE;
    prefs.port = 0;
    prefs.autoConnect = 1;
    pset(prefs.init, "ATE0V1", 63);
    pset(prefs.system, DEFAULT_SYSTEM, 255);
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
    if (FSOpen("\pAsk AI Prefs", prefsVol(), &ref) != noErr) return;
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
    e = FSOpen("\pAsk AI Prefs", vol, &ref);
    if (e == fnfErr) {
        e = Create("\pAsk AI Prefs", vol, 'AskA', 'pref');
        if (e == noErr) e = FSOpen("\pAsk AI Prefs", vol, &ref);
    }
    if (e == noErr) {
        e = FSWrite(ref, &n, &prefs);
        SetEOF(ref, sizeof prefs);
        FSClose(ref);
        FlushVol(NULL, vol);
    }
    if (e != noErr && !prefsSaveFailed) {
        prefsSaveFailed = 1;
        notec("(Settings couldn't be saved: is the disk locked? They last until you quit.)");
    }
}

/* ------------------------------------------------------------------ hooks called by link.c */
static unsigned long lastRx;

void ui_rxBytes(long n)
{
    rxTotal += n;
    dbg.rxBytes = rxTotal;
    lastRx = TickCount();
}

void ui_note(const char *s) { SetPort(win); notec(s); }

void ui_error(short code, const char *s)
{
    SetPort(win);
    dbg.errors++;
    dbg.lastErr = code;
    notec(s);
}

static char replyTag[10];
static short replyRowMade;

void ui_beginReply(const char *tag)      /* the row appears with the first text (none if the reply fails) */
{
    strncpy(replyTag, tag[0] ? tag : "AI", sizeof replyTag - 1);
    replyTag[sizeof replyTag - 1] = 0;
    replyRowMade = 0;
    inReply = 1;
}

static void replyRow(void)
{
    if (!replyRowMade) {
        replyRowMade = 1;
        flushDraw();
        newRow(RK_AI, replyTag[0] ? replyTag : "AI");
        needRow = 0;
    }
}

void ui_text(const char *s, short n)
{
    SetPort(win);
    if (!inReply) { replyTag[0] = 0; inReply = 1; replyRowMade = 0; }
    replyRow();
    if (needRow) { newRow(RK_TEXT, NULL); needRow = 0; }
    putText(s, n);
    {   /* keep the end of the reply for test scripts */
        short k = n > 63 ? 63 : n, have = (short)strlen(dbg.lastText);
        if (have + k > 63) {
            short drop = (short)(have + k - 63);
            BlockMove(dbg.lastText + drop, dbg.lastText, have - drop);
            have = (short)(have - drop);
        }
        memcpy(dbg.lastText + have, s + n - k, k);
        dbg.lastText[have + k] = 0;
    }
}

void ui_newline(void)
{
    SetPort(win);
    replyRow();
    if (needRow) needRow = 0;
    newRow(RK_TEXT, NULL);
}

void ui_endReply(const char *stop)
{
    SetPort(win);
    flushDraw();
    inReply = 0;
    if (!strcmp(stop, "length")) notec("(The reply reached the length limit.)");
    else if (!strcmp(stop, "stopped")) notec("(Stopped.)");
    else if (!strcmp(stop, "lost")) { /* the error note says it */ }
    if (strcmp(stop, "lost") && strcmp(stop, "error")) dbg.replies++;
    dbg.replyBytes = replyBytes;
    if (replyTicks > 0) lastBps = replyBytes * 60 / replyTicks;
    dbg.bps = lastBps;
    dbg.serErrors = ser_errors();
    drawStatus();
}

void ui_termText(const char *s, short n)   /* the modem's answers: CR starts a new row, LF is ignored */
{
    short i, k = 0;
    char out[256];
    SetPort(win);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\r') {
            if (k && termRow) putText(out, k);
            k = 0;
            if (termRow) newRow(RK_TEXT, NULL);
            continue;
        }
        if (c < 32 || c == 127) continue;
        if (!termRow) { blankBeforeTurn(); newRow(RK_AI, "Modem"); termRow = 1; }
        out[k++] = (char)c;
    }
    if (k) putText(out, k);
}

static void updateMenus(void);

void ui_modelsChanged(void)
{
    short i, n = CountMItems(modelM);
    for (i = n; i >= 1; i--) DeleteMenuItem(modelM, i);
    if (!nModels) {
        AppendMenu(modelM, "\p(connect to see the models)");
        DisableItem(modelM, 1);
    }
    for (i = 0; i < nModels; i++) {
        unsigned char p[48];
        AppendMenu(modelM, "\px");                  /* then the text, which may hold metacharacters */
        pset(p, models[i].label, 40);
        SetMenuItemText(modelM, (short)(i + 1), p);
        CheckItem(modelM, (short)(i + 1), i == curModel);
    }
    dbg.nModels = nModels;
    memset(dbg.model, 0, sizeof dbg.model);
    if (curModel >= 0) {
        const char *id = models[curModel].id;
        strcpy(dbg.model, id);
        if (prefs.model[0] != strlen(id) || memcmp(prefs.model + 1, id, prefs.model[0])) {
            pset(prefs.model, id, 31);       /* remembered for next time */
            prefsDirty = 1;
        }
    }
    updateMenus();
    drawStatus();
}

void ui_stateChanged(void)
{
    dbg.state = linkState;
    updateMenus();
    drawStatus();
}

void ui_idle(void) {}

/* ------------------------------------------------------------------ menus */
static void updateMenus(void)
{
    static short shown = -1;                    /* the Model title's state as drawn */
    short on = linkState == L_READY && nModels;
    SetMenuItemText(connM, 1, linkState == L_OFF ? "\pConnect" : "\pDisconnect");
    if (linkState == L_REPLY) EnableItem(connM, 2); else DisableItem(connM, 2);
    if (on) EnableItem(modelM, 0); else DisableItem(modelM, 0);
    if (linkState == L_OFF) EnableItem(connM, 4); else DisableItem(connM, 4);
    if (on != shown) { shown = on; DrawMenuBar(); }   /* only the Model title changes look */
}

static void sendMessage(const char *text, short n)
{
    SetPort(win);
    blankBeforeTurn();
    newRow(RK_YOU, "You");
    needRow = 0;
    putText(text, n);
    inReply = 0;
    dbg.lastText[0] = 0;
    if (link_ask(text, n)) { dbg.sent++; dbg.txBytes += n + 5; }
    drawStatus();
}

static void submit(void)
{
    if (!inLen) return;
    if (linkState == L_TERM) {                 /* the modem terminal: the line goes to the modem */
        SetPort(win);
        blankBeforeTurn();
        newRow(RK_YOU, "You");
        putText(input, inLen);
        termRow = 0;
        link_termSend(input, inLen);
        inLen = 0;
        drawInput();
        return;
    }
    if (linkState == L_READY && !queuedLen) {
        short n = inLen;
        char t[400];
        memcpy(t, input, n);
        inLen = 0;
        drawInput();
        sendMessage(t, n);
    } else if (linkState >= L_WAKE && linkState <= L_REPLY) {
        if (queuedLen) { SysBeep(5); return; }   /* one message is already waiting */
        memcpy(queued, input, inLen);             /* sent as soon as the reply (or connecting) is done */
        queuedLen = inLen;
        inLen = 0;
        drawInput();
        drawStatus();
    } else {
        notec("Not connected: choose Connection > Connect (\021K) first.");
        SysBeep(5);
    }
}

static void newChat(void)
{
    link_newChat();
    nRows = 0;
    scrollTop = 0;
    inReply = needRow = 0;
    queuedLen = 0;
    drawAll();
    setScrollBar();
    notec(link_online() ? "New chat: the model starts afresh." : "New chat.");
}

static void doSettings(void);

static void quitApp(void)
{
    unsigned long t0 = TickCount();
    EventRecord ev;
    quitting = 1;
    if (linkState != L_OFF) {
        link_disconnect();
        while (linkState != L_OFF && TickCount() - t0 < 60 * 8) {
            link_poll();
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
        if (item == 1) Alert(130, NULL);
        else if (item > 2) {
            Str255 name;
            GetMenuItemText(appleM, item, name);
            OpenDeskAcc(name);
            SetPort(win);
        }
    } else if (menu == 129) {
        if (item == 1) newChat();
        else if (item == 3) quitApp();
    } else if (menu == 130) {
        if (!SystemEdit((short)(item - 1))) {
            if (item == 3 || item == 4) {               /* Cut, Copy: the input line */
                ZeroScrap();
                PutScrap(inLen, 'TEXT', input);
                if (item == 3) { inLen = 0; drawInput(); }
            } else if (item == 5) {                      /* Paste into the input line */
                Handle h = NewHandle(0);
                long off, n = GetScrap(h, 'TEXT', &off), i;
                if (n > 0) {
                    HLock(h);
                    for (i = 0; i < n && inLen < (short)sizeof input - 1; i++) {
                        char c = (*h)[i];
                        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
                        if ((unsigned char)c >= 32 && c != 127) input[inLen++] = c;
                    }
                    HUnlock(h);
                    drawInput();
                }
                DisposeHandle(h);
            } else if (item == 6) { inLen = 0; drawInput(); }
        }
    } else if (menu == 131) {
        if (item == 1) {
            if (linkState == L_OFF) link_connect();
            else link_disconnect();
        } else if (item == 2) link_stop();
        else if (item == 4) {
            link_terminal();
            if (linkState == L_TERM)
                notec("Modem terminal: what you type goes straight to the modem (try AT, then ATI). "
                      "Type +++ alone to get back to its commands. Connection > Disconnect ends it.");
        } else if (item == 5) doSettings();
    } else if (menu == 132) {
        link_use((short)(item - 1));
    }
    HiliteMenu(0);
}

/* ------------------------------------------------------------------ settings dialog */
enum { SI_OK = 1, SI_CANCEL, SI_DEFAULTS, SI_DIAL = 5, SI_INIT = 7, SI_PIN = 9, SI_BAUD = 11, SI_FLOW = 18,
       SI_PORT = 22, SI_AUTO = 24, SI_SYSTEM = 26, SI_OUTLINE = 27 };

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
    GrafPtr old;
    (void)d;
    GetPort(&old);
    SetPort(win);
    link_poll();                    /* replies keep arriving behind the dialog */
    SetPort(old);
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

static void fillDialog(DialogPtr d, const Prefs *p)
{
    setText(d, SI_DIAL, p->dial);
    setText(d, SI_INIT, p->init);
    setText(d, SI_PIN, p->pin);
    setText(d, SI_SYSTEM, p->system);
    setRadios(d, p);
}

static void readTexts(DialogPtr d, Prefs *p)
{
    getText(d, SI_DIAL, p->dial, 63);
    getText(d, SI_INIT, p->init, 63);
    getText(d, SI_PIN, p->pin, 31);
    getText(d, SI_SYSTEM, p->system, 255);
}

static void doSettings(void)
{
    DialogPtr d = GetNewDialog(200, NULL, (WindowPtr)-1);
    Prefs p = prefs, old = prefs;
    short item = 0, t, w, h;
    Handle hd;
    Rect r;
    if (!d) { SysBeep(5); return; }
    w = (short)(d->portRect.right - d->portRect.left);
    h = (short)(d->portRect.bottom - d->portRect.top);
    MoveWindow(d, (short)((scr.left + scr.right - w) / 2),
               (short)(LMGetMBarHeight() + (scr.bottom - LMGetMBarHeight() - h) / 3), false);
    GetDialogItem(d, SI_OUTLINE, &t, &hd, &r);
    SetDialogItem(d, SI_OUTLINE, userItem, (Handle)drawOutline, &r);
    fillDialog(d, &p);
    SelectDialogItemText(d, SI_DIAL, 0, 32767);
    ShowWindow(d);
    for (;;) {
        ModalDialog((ModalFilterUPP)settingsFilter, &item);
        if (item == SI_OK || item == SI_CANCEL) break;
        if (item == SI_DEFAULTS) {                  /* modem settings back to the defaults; address and PIN stay */
            Prefs keep = prefs;
            readTexts(d, &p);
            defaultPrefs();
            memcpy(prefs.dial, p.dial, sizeof p.dial);
            memcpy(prefs.pin, p.pin, sizeof p.pin);
            p = prefs;
            prefs = keep;
            fillDialog(d, &p);
            continue;
        }
        if (item >= SI_BAUD && item < SI_BAUD + NBAUDS) p.baud = (short)(item - SI_BAUD);
        else if (item >= SI_FLOW && item < SI_FLOW + 3) p.flow = (short)(item - SI_FLOW);
        else if (item >= SI_PORT && item < SI_PORT + 2) p.port = (short)(item - SI_PORT);
        else if (item == SI_AUTO) p.autoConnect = (short)!p.autoConnect;
        setRadios(d, &p);
    }
    if (item == SI_OK) {
        readTexts(d, &p);
        memcpy(p.model, prefs.model, sizeof p.model);
        prefs = p;
        prefsDirty = 1;
    }
    DisposeDialog(d);
    SetPort(win);
    if (item == SI_OK) {
        if (link_online() && memcmp(old.system, prefs.system, sizeof prefs.system)) {
            if (linkState == L_READY) link_sendSystem();
        }
        if (linkState != L_OFF && (old.baud != prefs.baud || old.flow != prefs.flow || old.port != prefs.port
                                   || memcmp(old.dial, prefs.dial, sizeof old.dial)
                                   || memcmp(old.init, prefs.init, sizeof old.init)
                                   || memcmp(old.pin, prefs.pin, sizeof old.pin)))
            notec("(The new modem and relay settings apply the next time you connect.)");
    }
}

/* ------------------------------------------------------------------ keys and mouse */
static pascal void scrollAction(ControlHandle c, short part)
{
    short d = 0;
    if (part == inUpButton) d = -1;
    else if (part == inDownButton) d = 1;
    else if (part == inPageUp) d = (short)-(visLines - 1);
    else if (part == inPageDown) d = (short)(visLines - 1);
    if (d) {
        scrollTo((short)(scrollTop + d));
        SetControlValue(c, scrollTop);
    }
}

static void doMouse(EventRecord *ev)
{
    WindowPtr w;
    short part = FindWindow(ev->where, &w);
    if (part == inMenuBar) doMenu(MenuSelect(ev->where));
    else if (part == inSysWindow) SystemClick(ev, w);
    else if (part == inContent && w == win) {
        ControlHandle c;
        Point pt = ev->where;
        SetPort(win);
        GlobalToLocal(&pt);
        part = FindControl(pt, win, &c);
        if (c == sbar && part) {
            if (part == inThumb) {
                if (TrackControl(c, pt, NULL)) scrollTo(GetControlValue(c));
            } else TrackControl(c, pt, (ControlActionUPP)scrollAction);
        }
    }
}

static void doKey(EventRecord *ev)
{
    char c = (char)(ev->message & charCodeMask);
    short code = (short)((ev->message & keyCodeMask) >> 8);
    if (ev->modifiers & cmdKey) {
        if (c == '.') { link_stop(); return; }
        doMenu(MenuKey(c));
        return;
    }
    if (c == 30 || c == 31 || code == 0x4D || code == 0x48) {   /* up / down arrows: scroll */
        short d = (short)((c == 30 || code == 0x4D) ? -1 : 1);
        if (ev->modifiers & optionKey) d = (short)(d * (visLines - 1));
        scrollTo((short)(scrollTop + d));
        return;
    }
    if (c == '\r' || c == 3) { submit(); return; }
    if (c == 8) { if (inLen) { inLen--; drawInput(); } return; }
    if ((unsigned char)c >= 32 && c != 127 && inLen < (short)sizeof input - 1) {
        input[inLen++] = c;
        drawInput();
    }
}

/* ------------------------------------------------------------------ main */
int main(void)
{
    EventRecord ev;
    Rect r;

    dbg.magic = MAGIC;
    dbg.version = 1;
    dbg.notMagic = ~MAGIC;

    MaxApplZone();
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
    modelM = NewMenu(132, "\pModel");
    InsertMenu(modelM, 0);

    r = qd.screenBits.bounds;
    win = NewWindow(NULL, &r, "\p", true, plainDBox, (WindowPtr)-1, false, 0);
    SetPort(win);
    layout();
    sbar = NewControl(win, &sbRect, "\p", true, 0, 0, 0, scrollBarProc, 0);
    loadPrefs();
    ui_modelsChanged();
    drawAll();
    ValidRect(&win->portRect);

    if (!prefs.dial[0]) {
        notec("Welcome to Ask AI! Choose Connection > Settings\311 and enter your relay's address "
              "(for example 192.168.1.20:6800) and PIN, then Connection > Connect.");
    } else {
        notec("Type a message and press Return. Connection > Connect (\021K) dials the relay.");
        if (prefs.autoConnect) link_connect();
    }
    dbg.freeMem = FreeMem();

    for (;;) {
        SystemTask();
        link_poll();
        if (queuedLen && linkState == L_READY) {
            short n = queuedLen;
            queuedLen = 0;
            sendMessage(queued, n);
        }
        if (linkState == L_REPLY && TickCount() - lastStatusTick > 30) drawStatus();
        if (prefsDirty && (linkState == L_OFF || (linkState == L_READY && TickCount() - lastRx > 60))) savePrefs();
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
            EndUpdate((WindowPtr)ev.message);
            break;
        default:
            break;
        }
    }
    return 0;
}

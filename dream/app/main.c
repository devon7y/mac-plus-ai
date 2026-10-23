/*
 * Dream: a tiny diffusion model that paints clip art on a Mac Plus (4 MB, System 6) or SE.
 * Pick a subject (type to filter) and click Dream: the picture develops out of static, one
 * network pass per step, redrawn after every step. Click anywhere to stop after the current step.
 *
 * Engine: engine.c (model file, compiled layers, sampler; bit-exact with engine/picture.py).
 * This file: Toolbox setup, window layout, picker, menus, drawing, Copy / Save as MacPaint,
 * and the debug block + mailbox that the MAME tests use (magic 'DREM').
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
#include <Resources.h>
#include <Files.h>
#include <StandardFile.h>
#include <ToolUtils.h>
#include <SegLoad.h>
#include <Multiverse.h>
#include "engine.h"

#define APP_W 384
#define PIC 192                 /* picture on screen: 64 x 64 at 3x */
#define ROWS 13                 /* picker rows */

/* ------------------------------------------------------------------ debug block (tests) */
struct DebugBlock {
    unsigned long magic;        /*   0 'DREM' */
    unsigned long version;      /*   4 */
    unsigned long notMagic;     /*   8 */
    unsigned long state;        /*  12 0 start 1 loading 2 ready 3 dreaming 4 done 5 load error 6 stopped */
    long loadErr;               /*  16 */
    unsigned long loadTicks;    /*  20 */
    unsigned long freeMem;      /*  24 after loading */
    unsigned long modelRam;     /*  28 */
    unsigned long codeBytes;    /*  32 */
    unsigned long subject;      /*  36 */
    unsigned long seed;         /*  40 */
    unsigned long step;         /*  44 steps done */
    unsigned long nsteps;       /*  48 */
    unsigned long x0;           /*  52 address of the current x0 (npix int16, storage order) */
    unsigned long npix;         /*  56 */
    unsigned long stepTicks[16];/*  60 ticks per step */
    unsigned long cmd;          /* 124 mailbox: 1 = dream arg0 subject, arg1 seed; 2 = save "Test<arg0>" */
    unsigned long arg[4];       /* 128 */
    unsigned long cmdDone;      /* 144 */
    unsigned long cmdResult;    /* 148 */
    unsigned long dreams;       /* 152 pictures finished */
    unsigned long beginTicks;   /* 156 ticks for dm_begin (subject biases + noise) */
    unsigned long scrapLen;     /* 160 */
    unsigned long fileOps;      /* 164 */
    unsigned long picSum;       /* 168 checksum of the final x0 */
    Rect pic, list, query, dreamBtn, surpBtn;   /* 172 180 188 196 204: screen rectangles */
    unsigned long readTicks;    /* 212 ticks spent in FSRead while loading */
    unsigned long wgtsTicks;    /* 216 ticks from the first to the last compile progress call */
};
struct DebugBlock dbg;
#define DBG_MAGIC 0x4452454DUL

static DreamModel gM;
static WindowPtr gWin;
static short gOX, gOY;
static Rect gPicR, gQueryR, gListR, gUpR, gDownR, gDreamR, gSurpR, gStatusR, gBarR, gSeedR;
static MenuHandle mApple, mFile, mDream;
static BitMap gBM;                       /* the 192 x 192 picture */
static Boolean gHavePic, gStop;
static short gLastSubject = -1;
static unsigned long gLastSeed, gRand = 0x13579BDFUL;
static unsigned char gTitle[64] = "\pUntitled";

enum { M_APPLE = 128, M_FILE, M_DREAM };
enum { I_COPY = 1, I_SAVE, I_SEP1, I_QUIT };
enum { I_DREAM = 1, I_SURPRISE, I_AGAIN };

/* ------------------------------------------------------------------ helpers */
static unsigned long Rand(void)
{
    gRand ^= gRand << 13;
    gRand ^= gRand >> 17;
    gRand ^= gRand << 5;
    return gRand;
}

static void NumStr(unsigned long v, unsigned char *s)
{
    unsigned char t[12];
    short n = 0, i;
    do { t[n++] = (unsigned char)('0' + v % 10); v /= 10; } while (v);
    s[0] = (unsigned char)n;
    for (i = 0; i < n; i++) s[1 + i] = t[n - 1 - i];
}

static void Cat(unsigned char *s, const unsigned char *t)
{
    short i;
    for (i = 1; i <= t[0] && s[0] < 250; i++) s[++s[0]] = t[i];
}

static unsigned char gStatusText[256];

static void SetStatus(const unsigned char *msg)
{
    GrafPtr old;
    short i;
    for (i = 0; i <= msg[0]; i++) gStatusText[i] = msg[i];
    GetPort(&old);
    SetPort(gWin);
    EraseRect(&gStatusR);
    TextFont(3);
    TextSize(9);
    MoveTo(gStatusR.left + 2, gStatusR.bottom - 3);
    DrawString(msg);
    TextFont(0);
    TextSize(0);
    SetPort(old);
}

static short gBarX = -1;                 /* right edge of the filled part, -1 = not drawn */

static void Bar(long done, long total)
{
    Rect r = gBarR, f;
    short x;
    InsetRect(&r, 1, 1);
    x = (short)(total > 0 ? r.left + (long)(r.right - r.left) * done / total : r.left);
    if (gBarX < 0 || x < gBarX) {         /* full redraw */
        FrameRect(&gBarR);
        EraseRect(&r);
        gBarX = r.left;
    }
    if (x > gBarX) {                      /* paint only the new part */
        f = r;
        f.left = gBarX;
        f.right = x;
        PaintRect(&f);
        gBarX = x;
    }
}

/* ------------------------------------------------------------------ picture */
static const unsigned char ORDER[3][3] = {{6, 1, 5}, {3, 0, 8}, {7, 4, 2}};

/* Draw a G x G x P*P storage-order Q9 picture into gBM as halftone cells: 3 x 3 for 64 x 64
   pictures, the same pattern at 2 x 2 per sub-cell (6 x 6) for 32 x 32 ones. Each screen row is
   built 24 bits at a time from per-level bit patterns (8 pixels x 3 bits, or 4 pixels x 6 bits). */
static void RenderQ9(const short *v)
{
    short G = gM.TH[0], P = gM.patch, C = gM.TC[0], rb = gBM.rowBytes, N = G * P, S = PIC / N;
    short Y, X, w, k;
    unsigned char *base = (unsigned char *)gBM.baseAddr;
    static unsigned char lev[1025], g[64];
    static unsigned short pat[10][6];       /* bits of one screen row of a cell (black = 1) */
    static short ready = -1;
    long i;
    if (ready != S) {
        for (i = 0; i <= 1024; i++) {   /* contrast stretch (FORMAT.md, display) */
            long t = (i - 123) * 10;
            t = t < 0 ? 0 : t / 778;
            lev[i] = (unsigned char)(t > 9 ? 9 : t);
        }
        for (k = 0; k < 10; k++)
            for (w = 0; w < S; w++) {
                unsigned short bits = 0;
                short u;
                for (u = 0; u < S; u++)
                    bits = (unsigned short)(bits << 1 | (ORDER[w * 3 / S][u * 3 / S] >= k));
                pat[k][w] = bits;
            }
        ready = S;
    }
    for (Y = 0; Y < N; Y++) {
        const short *row = v + ((long)(Y / P) * G * C) + (Y % P) * P;
        {                                 /* levels of this picture row (no divisions) */
            short gx, px;
            unsigned char *gp = g;
            for (gx = 0; gx < G; gx++, row += C)
                for (px = 0; px < P; px++) {
                    short q = row[px] + 512;
                    *gp++ = lev[q < 0 ? 0 : q > 1024 ? 1024 : q];
                }
        }
        for (w = 0; w < S; w++) {
            unsigned char *out = base + (long)(Y * S + w) * rb;
            short per = 24 / S;           /* pixels per 24 bits: 8 (S = 3) or 4 (S = 6) */
            for (X = 0; X < N; X += per) {
                unsigned long acc = 0;
                for (k = 0; k < per; k++) acc = acc << S | pat[g[X + k]][w];
                *out++ = (unsigned char)(acc >> 16);
                *out++ = (unsigned char)(acc >> 8);
                *out++ = (unsigned char)acc;
            }
        }
    }
}

static void DrawPic(void)
{
    GrafPtr old;
    Rect src;
    GetPort(&old);
    SetPort(gWin);
    SetRect(&src, 0, 0, PIC, PIC);
    if (gHavePic)
        CopyBits(&gBM, &gWin->portBits, &src, &gPicR, srcCopy, NULL);
    else
        EraseRect(&gPicR);
    SetPort(old);
}

/* ------------------------------------------------------------------ subject picker */
static short pkMatch[DM_MAXSUBJ], pkN, pkSel, pkTop, pkQLen;
static Boolean pkPicked;     /* the user chose a row (click or arrow keys) since the query last changed */
static unsigned char pkQuery[24];

static Boolean Contains(const unsigned char *name, const unsigned char *q, short ql)
{
    short i, j, n = name[0];
    for (i = 0; i + ql <= n; i++) {
        for (j = 0; j < ql; j++) {
            unsigned char c = name[1 + i + j];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != q[j]) break;
        }
        if (j == ql) return true;
    }
    return false;
}

static void PkFilter(void)
{
    short i;
    pkN = 0;
    for (i = 0; i < gM.nsub; i++)
        if (Contains(gM.name[i], pkQuery, pkQLen)) pkMatch[pkN++] = i;
    pkSel = 0;
    pkTop = 0;
    pkPicked = false;
}

static void PkDrawList(void)
{
    short r;
    Rect row;
    EraseRect(&gListR);
    TextFont(3);
    TextSize(9);
    for (r = 0; r < ROWS && pkTop + r < pkN; r++) {
        SetRect(&row, gListR.left + 1, gListR.top + 1 + r * 12, gListR.right - 1, gListR.top + 13 + r * 12);
        MoveTo(row.left + 4, row.bottom - 3);
        DrawString(gM.name[pkMatch[pkTop + r]]);
        if (pkTop + r == pkSel) InvertRect(&row);
    }
    if (pkN == 0) {
        MoveTo(gListR.left + 6, gListR.top + 12);
        DrawString("\p(nothing matches)");
    }
    FrameRect(&gListR);
    TextFont(0);
    TextSize(0);
}

static void PkDrawQuery(void)
{
    Rect r = gQueryR;
    unsigned char s[26];
    short i;
    EraseRect(&r);
    FrameRect(&r);
    s[0] = (unsigned char)pkQLen;
    for (i = 0; i < pkQLen; i++) s[1 + i] = pkQuery[i];
    TextFont(3);
    TextSize(9);
    MoveTo(r.left + 4, r.bottom - 4);
    DrawString(s);
    MoveTo(r.left + 4 + StringWidth(s), r.top + 3);
    Line(0, r.bottom - r.top - 7);
    TextFont(0);
    TextSize(0);
}

static void PkShowSel(void)
{
    if (pkSel < pkTop) pkTop = pkSel;
    if (pkSel >= pkTop + ROWS) pkTop = pkSel - ROWS + 1;
    PkDrawList();
}

static short Selected(void)
{
    return pkN > 0 ? pkMatch[pkSel] : -1;
}

/* ------------------------------------------------------------------ drawing */
static void DrawButton(const Rect *r, const unsigned char *label, Boolean dflt)
{
    Rect b = *r;
    EraseRect(&b);
    FrameRoundRect(&b, 10, 10);
    if (dflt) {
        InsetRect(&b, -3, -3);
        PenSize(2, 2);
        FrameRoundRect(&b, 14, 14);
        PenNormal();
    }
    MoveTo(r->left + (r->right - r->left - StringWidth(label)) / 2, r->bottom - 5);
    DrawString(label);
}

static void DrawSeed(void)
{
    unsigned char s[40] = "\pSeed ";
    unsigned char n[12];
    EraseRect(&gSeedR);
    if (gLastSubject < 0) return;
    NumStr(gLastSeed, n);
    Cat(s, n);
    TextFont(3);
    TextSize(9);
    MoveTo(gSeedR.left + 2, gSeedR.bottom - 3);
    DrawString(s);
    TextFont(0);
    TextSize(0);
}

static void DrawAll(void)
{
    Rect fr = gPicR;
    SetPort(gWin);
    EraseRect(&gWin->portRect);
    InsetRect(&fr, -2, -2);
    FrameRect(&fr);
    DrawPic();
    TextFont(0);
    TextSize(0);
    MoveTo(gQueryR.left, gQueryR.top - 5);
    DrawString("\pSubject (type to search):");
    PkDrawQuery();
    PkDrawList();
    FrameRect(&gUpR);
    FrameRect(&gDownR);
    {
        short x = (gUpR.left + gUpR.right) / 2, y = gUpR.top + 5, k;
        for (k = 0; k < 5; k++) { MoveTo(x - k, y + k); LineTo(x + k, y + k); }
        y = gDownR.bottom - 6;
        for (k = 0; k < 5; k++) { MoveTo(x - k, y - k); LineTo(x + k, y - k); }
    }
    DrawButton(&gDreamR, "\pDream", true);
    DrawButton(&gSurpR, "\pSurprise me", false);
    DrawSeed();
    gBarX = -1;
    Bar(0, 0);
    if (gStatusText[0]) SetStatus(gStatusText);
}

static void Layout(void)
{
    Rect b = qd.screenBits.bounds;
    gOX = (short)((b.left + b.right - APP_W) / 2);
    gOY = 20;
    SetRect(&gPicR, gOX + 10, gOY + 12, gOX + 10 + PIC, gOY + 12 + PIC);
    SetRect(&gQueryR, gOX + 214, gOY + 24, gOX + 376, gOY + 42);
    SetRect(&gListR, gOX + 214, gOY + 46, gOX + 358, gOY + 48 + ROWS * 12);
    SetRect(&gUpR, gOX + 360, gListR.top, gOX + 376, gListR.top + 20);
    SetRect(&gDownR, gOX + 360, gListR.bottom - 20, gOX + 376, gListR.bottom);
    SetRect(&gDreamR, gOX + 10, gPicR.bottom + 14, gOX + 90, gPicR.bottom + 34);
    SetRect(&gSurpR, gOX + 104, gPicR.bottom + 14, gOX + 202, gPicR.bottom + 34);
    SetRect(&gSeedR, gOX + 214, gListR.bottom + 6, gOX + 376, gListR.bottom + 20);
    SetRect(&gBarR, gOX + 214, gPicR.bottom + 20, gOX + 376, gPicR.bottom + 28);
    SetRect(&gStatusR, gOX + 2, gDreamR.bottom + 8, gOX + APP_W, gDreamR.bottom + 22);
    dbg.pic = gPicR; dbg.list = gListR; dbg.query = gQueryR; dbg.dreamBtn = gDreamR; dbg.surpBtn = gSurpR;
}

/* ------------------------------------------------------------------ model loading */
static short rRef;
static unsigned char rBuf[4096];
static short rPos, rLen;
static OSErr rErr;

static long ReadModel(void *ctx, void *dst, long n)
{
    unsigned char *d = (unsigned char *)dst;
    long got = 0;
    (void)ctx;
    while (got < n && !rErr) {
        if (rPos >= rLen) {
            long cnt = sizeof rBuf;
            unsigned long t0 = TickCount();
            rErr = FSRead(rRef, &cnt, rBuf);
            dbg.readTicks += TickCount() - t0;
            if (rErr == eofErr && cnt > 0) rErr = noErr;
            rLen = (short)cnt;
            rPos = 0;
            if (cnt == 0) break;
            continue;
        }
        {
            long k = rLen - rPos;
            if (k > n - got) k = n - got;
            BlockMove(rBuf + rPos, d + got, k);
            rPos += (short)k;
            got += k;
        }
    }
    return got;
}

static OSErr HOpenRead(ConstStr255Param name, short vref, long dir, short *ref)
{
    HParamBlockRec pb;
    OSErr err;
    short i;
    for (i = 0; i < (short)sizeof pb; i++) ((char *)&pb)[i] = 0;
    pb.ioParam.ioNamePtr = (StringPtr)name;
    pb.ioParam.ioVRefNum = vref;
    pb.ioParam.ioPermssn = fsRdPerm;
    pb.fileParam.ioDirID = dir;
    err = PBHOpenSync(&pb);
    *ref = pb.ioParam.ioRefNum;
    return err;
}

/* "Dream Model": in the application's folder, else the default folder, else the volume root */
static OSErr OpenModel(short *ref)
{
    static const unsigned char name[] = "\pDream Model";
    FCBPBRec fcb;
    short i, appRef;
    __asm__ volatile("move.w 0x900.w,%0" : "=d"(appRef));     /* CurApRefNum */
    for (i = 0; i < (short)sizeof fcb; i++) ((char *)&fcb)[i] = 0;
    fcb.ioRefNum = appRef;
    if (PBGetFCBInfoSync(&fcb) == noErr && HOpenRead(name, fcb.ioFCBVRefNum, fcb.ioFCBParID, ref) == noErr)
        return noErr;
    if (FSOpen(name, 0, ref) == noErr)
        return noErr;
    if (fcb.ioFCBVRefNum && HOpenRead(name, fcb.ioFCBVRefNum, 2, ref) == noErr)
        return noErr;
    return fnfErr;
}

static void LoadProgress(long done, long total)
{
    static short last = -1;
    static unsigned long first;
    short pct = (short)(total ? done * 100 / total : 0);
    if (!first) first = TickCount();
    dbg.wgtsTicks = TickCount() - first;
    if (pct != last) {
        unsigned char s[48] = "\pLoading Dream's model: compiling ";
        unsigned char n[8];
        last = pct;
        NumStr(pct, n);
        Cat(s, n);
        Cat(s, "\p%");
        SetStatus(s);
        SetPort(gWin);
        Bar(done, total);
    }
}

static Boolean LoadModel(void)
{
    unsigned long t0 = TickCount();
    dbg.state = 1;
    SetStatus("\pLoading Dream's model\xC9");
    rErr = OpenModel(&rRef);
    if (rErr) {
        dbg.loadErr = rErr;
        dbg.state = 5;
        SetStatus("\pCan't find the file \"Dream Model\" next to Dream.");
        return false;
    }
    rPos = rLen = 0;
    dbg.loadErr = dm_load(&gM, ReadModel, NULL, LoadProgress);
    FSClose(rRef);
    dbg.loadTicks = TickCount() - t0;
    if (dbg.loadErr) {
        dbg.state = 5;
        SetStatus(dbg.loadErr == 3 ? "\pNot enough memory for Dream's model (needs a 4 MB Mac)."
                                : "\pDream's model file is damaged.");
        return false;
    }
    dbg.freeMem = FreeMem();
    dbg.modelRam = gM.ram;
    dbg.codeBytes = gM.codebytes;
    dbg.nsteps = gM.N;
    dbg.npix = gM.npix;
    dbg.x0 = (unsigned long)gM.x0;
    return true;
}

/* ------------------------------------------------------------------ dreaming */
static short gCurStep;

static void LayerTick(long done, long total)
{
    EventRecord ev;
    SetPort(gWin);
    Bar((long)gCurStep * total + done, (long)gM.N * total);
    if (!gStop && EventAvail(mDownMask, &ev)) {
        GetNextEvent(mDownMask, &ev);
        gStop = true;
        SetStatus("\pStopping after this step\xC9");
    }
}

static void Dream(short subject, unsigned long seed)
{
    unsigned long t0, ts;
    short i;
    unsigned char s[80], n[12];
    if (subject < 0 || subject >= gM.nsub) { SysBeep(5); return; }
    gLastSubject = subject;
    gLastSeed = seed;
    gStop = false;
    dbg.state = 3;
    dbg.subject = (unsigned long)subject;
    dbg.seed = seed;
    dbg.step = 0;
    gTitle[0] = 0;
    Cat(gTitle, gM.name[subject]);
    SetPort(gWin);
    DrawSeed();
    t0 = TickCount();
    dm_begin(&gM, subject, seed);
    dbg.beginTicks = TickCount() - t0;
    RenderQ9(gM.x);                      /* the static it starts from */
    gHavePic = true;
    DrawPic();
    for (i = 0; i < gM.N; i++) {
        s[0] = 0;
        Cat(s, "\pDreaming of ");
        Cat(s, gM.name[subject]);
        Cat(s, "\p\xC9 step ");
        NumStr(i + 1, n); Cat(s, n);
        Cat(s, "\p of ");
        NumStr(gM.N, n); Cat(s, n);
        if (i > 0) {                       /* time left, from the steps so far */
            unsigned long per = (TickCount() - t0) / i, left = (per * (gM.N - i) + 30) / 60;
            Cat(s, "\p (about ");
            NumStr(left, n); Cat(s, n);
            Cat(s, "\p s left)");
        }
        SetStatus(s);
        gCurStep = i;
        ts = TickCount();
        dm_step(&gM);
        dbg.stepTicks[i] = TickCount() - ts;
        dbg.step = i + 1;
        RenderQ9(gM.x0);
        DrawPic();
        if (gStop) break;
    }
    Bar(1, 1);
    {
        unsigned long sum = 0;
        long k;
        for (k = 0; k < gM.npix; k++) sum = sum * 31 + (unsigned short)gM.x0[k];
        dbg.picSum = sum;
    }
    s[0] = 0;
    Cat(s, gStop ? "\pStopped: " : "\pDone: ");
    Cat(s, gM.name[subject]);
    Cat(s, "\p (");
    NumStr((TickCount() - t0 + 30) / 60, n); Cat(s, n);
    Cat(s, "\p s). Dream again for another one.");
    SetStatus(s);
    dbg.state = gStop ? 6 : 4;
    dbg.dreams++;
    FlushEvents(mDownMask | mUpMask, 0);
}

static unsigned long NewSeed(void)
{
    unsigned long s;
    gRand ^= TickCount();
    s = Rand() % 1000000UL;
    return s ? s : 1;
}

/* ------------------------------------------------------------------ Copy, Save as MacPaint */
static PicHandle MakePicture(void)
{
    PicHandle pic;
    Rect src, fr;
    GrafPtr old;
    GetPort(&old);
    SetPort(gWin);
    SetRect(&src, 0, 0, PIC, PIC);
    fr = src;
    ClipRect(&fr);
    pic = OpenPicture(&fr);
    CopyBits(&gBM, &gWin->portBits, &src, &fr, srcCopy, NULL);
    ClosePicture();
    ClipRect(&gWin->portRect);
    SetPort(old);
    return pic;
}

static void CopyPic(void)
{
    PicHandle pic;
    long len;
    if (!gHavePic) { SysBeep(5); return; }
    pic = MakePicture();
    if (!pic) { SysBeep(5); return; }
    len = GetHandleSize((Handle)pic);
    ZeroScrap();
    HLock((Handle)pic);
    PutScrap(len, 'PICT', (Ptr)*pic);
    HUnlock((Handle)pic);
    KillPicture(pic);
    dbg.scrapLen = (unsigned long)len;
    SetStatus("\pCopied the picture to the Clipboard.");
}

#define PAGE_RB 72
#define PAGE_H 720
static OSErr WritePaint(ConstStr255Param name, short vRef)
{
    Ptr buf, row, dst;
    long len, count, i;
    short ref, y, x0 = (576 - PIC) / 2 / 8, top = 120;
    OSErr err;
    buf = NewPtrClear(512 + (long)PAGE_H * (PAGE_RB + 2));
    row = NewPtrClear(PAGE_RB);
    if (!buf || !row) return memFullErr;
    dst = buf + 512;                                   /* header: version 0, default patterns */
    for (y = 0; y < PAGE_H; y++) {
        Ptr src = row;
        for (i = 0; i < PAGE_RB; i++) row[i] = 0;
        if (y >= top && y < top + PIC)
            BlockMove((Ptr)gBM.baseAddr + (long)(y - top) * gBM.rowBytes, row + x0, PIC / 8);
        PackBits(&src, &dst, PAGE_RB);
    }
    len = dst - buf;
    err = Create(name, vRef, 'MPNT', 'PNTG');
    if (err == dupFNErr) err = noErr;
    if (!err) err = FSOpen(name, vRef, &ref);
    if (!err) {
        err = SetEOF(ref, 0);
        count = len;
        if (!err) err = FSWrite(ref, &count, buf);
        FSClose(ref);
        FlushVol(NULL, vRef);
    }
    DisposePtr(buf);
    DisposePtr(row);
    if (!err) dbg.fileOps++;
    return err;
}

static void SavePic(void)
{
    SFReply reply;
    Point p;
    if (!gHavePic) { SysBeep(5); return; }
    p.v = 60;
    p.h = gOX + 40;
    SFPutFile(p, "\pSave picture as:", gTitle, NULL, &reply);
    SetPort(gWin);
    if (!reply.good) return;
    if (WritePaint(reply.fName, reply.vRefNum) == noErr)
        SetStatus("\pSaved as a MacPaint document.");
    else {
        SysBeep(10);
        SetStatus("\pCould not save (disk full or locked?).");
    }
}

/* ------------------------------------------------------------------ menus, About */
static void SetupMenus(void)
{
    mApple = NewMenu(M_APPLE, "\p\x14");
    AppendMenu(mApple, "\pAbout Dream\xC9;(-");
    AppendResMenu(mApple, 'DRVR');
    InsertMenu(mApple, 0);
    mFile = NewMenu(M_FILE, "\pFile");
    AppendMenu(mFile, "\pCopy Picture/C;Save as MacPaint\xC9/S;(-;Quit/Q");
    InsertMenu(mFile, 0);
    mDream = NewMenu(M_DREAM, "\pDream");
    AppendMenu(mDream, "\pDream/D;Surprise Me/R;Again with a New Seed/N");
    InsertMenu(mDream, 0);
    DrawMenuBar();
}

static void About(void)
{
    WindowPtr w;
    Rect r;
    EventRecord ev;
    static const unsigned char *const lines[] = {
        "\pDream",
        "\pPaints clip art with a tiny diffusion model: about a",
        "\pmillion ternary weights, compiled to 68000 code at start.",
        "\p",
        "\pTrained on pictures made with FLUX 3 Image Klein by",
        "\pBlack Forest Labs (FLUX Kommunity License). Dream is a",
        "\pmodified, non-commercial derivative; it is not an official",
        "\pBlack Forest Labs product and is not endorsed by them.",
        "\p\"This FLUX Model is licensed by Black Forest Labs Inc.",
        "\punder the FLUX Kommunity License. Copyright Black Forest",
        "\pLabs Inc. IN NO EVENT SHALL BLACK FOREST LABS INC. BE",
        "\pLIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER",
        "\pIN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,",
        "\pOUT OF OR IN CONNECTION WITH USE OF THIS MODEL.\"",
        "\pSubject names: Google's Quick, Draw! categories.",
        "\p",
        "\pClick or press a key to continue.",
    };
    short i, n = sizeof lines / sizeof lines[0];
    SetRect(&r, gOX + 16, 40, gOX + APP_W - 16, 40 + 18 + n * 12);
    w = NewWindow(NULL, &r, "\p", true, dBoxProc, (WindowPtr)-1, false, 0);
    SetPort(w);
    for (i = 0; i < n; i++) {
        TextFont(i == 0 ? 0 : 3);
        TextSize(i == 0 ? 0 : 9);
        MoveTo(12, 16 + i * 12);
        DrawString(lines[i]);
    }
    dbg.cmdResult = 0xAB;
    for (;;) {
        SystemTask();
        if (GetNextEvent(mDownMask | keyDownMask, &ev)) break;
    }
    DisposeWindow(w);
    SetPort(gWin);
    TextFont(0);
    TextSize(0);
}

static void Surprise(void)
{
    if (gM.nsub <= 0) return;
    gRand ^= TickCount();
    Dream((short)(Rand() % (unsigned long)gM.nsub), NewSeed());
}

static void DoMenu(long mr)
{
    short menu = (short)(mr >> 16), item = (short)mr;
    if (menu == M_APPLE) {
        if (item == 1) About();
        else {
            Str255 name;
            GrafPtr old;
            GetPort(&old);
            GetMenuItemText(mApple, item, name);
            OpenDeskAcc(name);
            SetPort(old);
        }
    } else if (menu == M_FILE) {
        if (item == I_COPY) CopyPic();
        else if (item == I_SAVE) SavePic();
        else if (item == I_QUIT) ExitToShell();
    } else if (menu == M_DREAM && dbg.state >= 2 && dbg.state != 5) {
        if (item == I_DREAM) {
            if (pkQLen == 0 && !pkPicked) Surprise();   /* empty search box, nothing chosen: a surprise */
            else Dream(Selected(), NewSeed());
        }
        else if (item == I_SURPRISE) Surprise();
        else if (item == I_AGAIN) Dream(gLastSubject >= 0 ? gLastSubject : Selected(), NewSeed());
    }
    HiliteMenu(0);
}

static void Mailbox(void)
{
    unsigned long c = dbg.cmd;
    if (!c) return;
    dbg.cmd = 0;
    dbg.cmdResult = 0;
    if (c == 1 && dbg.state >= 2 && dbg.state != 5) {
        Dream((short)dbg.arg[0], dbg.arg[1]);
    } else if (c == 2) {
        unsigned char name[8] = "\pTest0";
        name[5] = (unsigned char)('0' + dbg.arg[0] % 10);
        dbg.cmdResult = (unsigned long)WritePaint(name, 0);
    } else if (c == 3) {
        CopyPic();
    }
    dbg.cmdDone++;
}

static void Key(unsigned char c)
{
    if (c == 13 || c == 3) {
        DoMenu((long)M_DREAM << 16 | I_DREAM);
    } else if (c == 8) {
        if (pkQLen > 0) { pkQLen--; PkFilter(); PkDrawQuery(); PkDrawList(); }
    } else if (c == 0x1E) {
        if (pkSel > 0) { pkSel--; pkPicked = true; PkShowSel(); }
    } else if (c == 0x1F) {
        if (pkSel < pkN - 1) { pkSel++; pkPicked = true; PkShowSel(); }
    } else if (c >= 32 && c < 127 && pkQLen < 23) {
        if (c >= 'A' && c <= 'Z') c += 32;
        pkQuery[pkQLen++] = c;
        PkFilter();
        PkDrawQuery();
        PkDrawList();
    }
}

static void Click(Point p, long when)
{
    static short lastRow = -1;
    static long lastClick;
    if (PtInRect(p, &gListR)) {
        short row = (p.v - gListR.top - 1) / 12;
        if (row >= 0 && row < ROWS && pkTop + row < pkN) {
            if (pkTop + row == lastRow && when - lastClick <= 30) {
                pkSel = pkTop + row;
                pkPicked = true;
                PkDrawList();
                DoMenu((long)M_DREAM << 16 | I_DREAM);
            } else {
                pkSel = pkTop + row;
                pkPicked = true;
                PkDrawList();
            }
            lastRow = pkTop + row;
            lastClick = when;
        }
    } else if (PtInRect(p, &gUpR)) {
        pkTop -= ROWS - 1;
        if (pkTop < 0) pkTop = 0;
        if (pkSel >= pkTop + ROWS) pkSel = pkTop + ROWS - 1;
        PkDrawList();
    } else if (PtInRect(p, &gDownR)) {
        if (pkTop + ROWS < pkN) pkTop += ROWS - 1;
        if (pkSel < pkTop) pkSel = pkTop;
        PkDrawList();
    } else if (PtInRect(p, &gDreamR)) {
        InvertRoundRect(&gDreamR, 10, 10);
        while (StillDown()) { }
        InvertRoundRect(&gDreamR, 10, 10);
        DoMenu((long)M_DREAM << 16 | I_DREAM);
    } else if (PtInRect(p, &gSurpR)) {
        InvertRoundRect(&gSurpR, 10, 10);
        while (StillDown()) { }
        InvertRoundRect(&gSurpR, 10, 10);
        DoMenu((long)M_DREAM << 16 | I_SURPRISE);
    }
}

int main(void)
{
    EventRecord ev;
    WindowPtr w;
    Rect r;
    short part;

    dbg.magic = DBG_MAGIC;
    dbg.version = 1;
    dbg.notMagic = ~DBG_MAGIC;
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

    Layout();
    SetupMenus();
    r = qd.screenBits.bounds;
    gWin = NewWindow(NULL, &r, "\p", true, plainDBox, (WindowPtr)-1, false, 0);
    SetPort(gWin);
    gBM.rowBytes = PIC / 8;
    SetRect(&gBM.bounds, 0, 0, PIC, PIC);
    gBM.baseAddr = NewPtrClear((long)gBM.rowBytes * PIC);
    DrawAll();
    SetCursor(*GetCursor(watchCursor));
    dm_layer_hook = LayerTick;
    if (LoadModel()) {
        pkQLen = 0;
        PkFilter();
        gStatusText[0] = 0;
        DrawAll();
        SetStatus("\pPick a subject and click Dream, or press Return for a surprise.");
        dbg.state = 2;
    }
    ValidRect(&gWin->portRect);
    InitCursor();
    gRand ^= TickCount();

    for (;;) {
        SystemTask();
        Mailbox();
        if (!GetNextEvent(everyEvent, &ev)) continue;
        switch (ev.what) {
        case mouseDown:
            part = FindWindow(ev.where, &w);
            if (part == inMenuBar) DoMenu(MenuSelect(ev.where));
            else if (part == inSysWindow) SystemClick(&ev, w);
            else if (w == gWin && dbg.state >= 2 && dbg.state != 5) { SetPort(gWin); Click(ev.where, ev.when); }
            break;
        case keyDown:
        case autoKey: {
            unsigned char c = (unsigned char)(ev.message & charCodeMask);
            if (ev.modifiers & cmdKey) {
                if (ev.what == keyDown) DoMenu(MenuKey((char)c));
            } else if (dbg.state >= 2 && dbg.state != 5) {
                SetPort(gWin);
                Key(c);
            }
            break;
        }
        case updateEvt:
            w = (WindowPtr)ev.message;
            BeginUpdate(w);
            if (w == gWin) DrawAll();
            EndUpdate(w);
            break;
        default:
            break;
        }
    }
    return 0;
}

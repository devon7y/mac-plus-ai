/*
 * Guess My Doodle: Quick, Draw! on a 1986 Macintosh. The Mac names a thing, you draw it with the
 * mouse in 20 seconds, and the Mac keeps guessing what it is while you draw.
 *
 * This file: the Toolbox setup, the layout, the game (title, round card, drawing, round over, end
 * screen with the six drawings), menus, About box and the debug block for the MAME tests.
 * The classifier is engine.c (68000 code generated from "Guess Model" at start-up); the drawing is
 * turned into its 32x32 input by raster.c; the mouse is sampled at 60 Hz by a VBL task (ink.c), so
 * the classifier can run in small steps between screen updates without losing any of a stroke.
 */
#include "guess.h"
#include "engine.h"
#include "raster.h"
#include "ink.h"
#include "sound.h"
#include "speech.h"

#define APP_W 448
#define CW 256                  /* canvas */
#define NROUNDS 6
#define ROUND_TICKS 1203L       /* 20 s at 60.15 Hz */
#define RESULT_TICKS 200L
#define MAXPTS 1500
#define MINSTEP 2               /* a new point once the mouse has moved this far */

enum { S_START, S_TITLE, S_CARD, S_DRAW, S_RESULT, S_END, S_ERROR = 9 };
enum { M_APPLE = 128, M_FILE, M_EDIT };
enum { I_NEW = 1, I_SEP1, I_SOUND, I_SPEAK, I_SEP2, I_QUIT };
enum { I_UNDO = 1, I_SEP3, I_CUT, I_COPY, I_PASTE, I_CLEAR, I_SEP4, I_CLEARALL };

struct DebugBlock dbg;
short gSoundOn = 1;

static GModel M;
static Ptr gFile;
static WindowPtr gWin;
static MenuHandle mApple, mFile, mEdit;
static Speech gSp;
static short gOX, gState;
static Rect rCanvas, rFrame, rTop, rPanel, rMsg, rB1, rB2, rB3, rStatus, rEndBtn;

/* the drawing of the current round */
static unsigned char px[MAXPTS], py[MAXPTS], pup[MAXPTS];
static short npts, penDown, drawVer, inferVer;
static short lastX, lastY;

/* the game */
typedef struct {
    short target, n, ok, secs;
    short top[3], pct[3];
} Round;
static Round rounds[NROUNDS];
static unsigned char sx[NROUNDS][MAXPTS], sy[NROUNDS][MAXPTS], sup[NROUNDS][MAXPTS];
static short gRound, gScore, gForce = -1, gTicked;
static unsigned long tDeadline, tResult, tLastSpeak, seed = 1;
static long tLeft;                      /* ticks left when the round ended */

/* guessing */
static short inferring, gTop[3] = {-1, -1, -1}, gPct[3], gSaid = -1;
static unsigned long tInfer;
static unsigned short expt[512];

/* ------------------------------------------------------------------ small helpers */
static unsigned long rnd(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static void PStr(unsigned char *d, const char *s)
{
    short n = 0;
    while (s[n] && n < 255) { d[n + 1] = (unsigned char)s[n]; n++; }
    d[0] = (unsigned char)n;
}

static void PCat(unsigned char *d, const unsigned char *s)
{
    short i;
    for (i = 1; i <= s[0] && d[0] < 255; i++) d[++d[0]] = s[i];
}

static void PCatC(unsigned char *d, const char *s)
{
    while (*s && d[0] < 255) d[++d[0]] = (unsigned char)*s++;
}

/* category k: its article ("a", "an", "the", or empty) and name; the model stores "a|bicycle" */
static void CatName(short k, unsigned char *art, unsigned char *nm)
{
    const unsigned char *p = g_name(&M, k);
    short i, bar = 0;
    for (i = 1; i <= p[0]; i++) if (p[i] == '|') { bar = i; break; }
    art[0] = 0;
    for (i = 1; i < bar; i++) art[++art[0]] = p[i];
    nm[0] = 0;
    for (i = bar + 1; i <= p[0]; i++) nm[++nm[0]] = p[i];
}

static void ArtName(short k, unsigned char *out)       /* "a bicycle" */
{
    unsigned char art[16], nm[64];
    CatName(k, art, nm);
    out[0] = 0;
    if (art[0]) { PCat(out, art); PCatC(out, " "); }
    PCat(out, nm);
}

static void Center(const unsigned char *s, short cx, short y)
{
    MoveTo(cx - StringWidth(s) / 2, y);
    DrawString(s);
}

static void Font(short f, short size, short face)
{
    TextFont(f);
    TextSize(size);
    TextFace(face);
}

static void DrawBtn(const Rect *r, const char *label, short enabled)
{
    unsigned char s[64];
    Rect b = *r;
    EraseRect(&b);
    PenSize(enabled ? 2 : 1, enabled ? 2 : 1);
    FrameRoundRect(&b, 12, 12);
    PenSize(1, 1);
    PStr(s, label);
    Font(0, 12, 0);
    Center(s, (short)((b.left + b.right) / 2), (short)(b.bottom - (b.bottom - b.top - 9) / 2 - 1));
    if (!enabled) {
        PenPat(&qd.gray);
        PenMode(patBic);
        PaintRect(&b);
        PenNormal();
        FrameRoundRect(&b, 12, 12);
    }
}

static int TrackButton(const Rect *r)
{
    int in = 1, was;
    Point p;
    InvertRoundRect(r, 12, 12);
    while (StillDown()) {
        GetMouse(&p);
        was = in;
        in = PtInRect(p, r);
        if (in != was) InvertRoundRect(r, 12, 12);
    }
    if (in) InvertRoundRect(r, 12, 12);
    return in;
}

static void Wrap(const unsigned char *s, const Rect *r)
{
    EraseRect(r);
    TETextBox((Ptr)(s + 1), s[0], r, teJustLeft);
}

/* ------------------------------------------------------------------ layout and drawing */
static void Layout(void)
{
    Rect b = qd.screenBits.bounds;
    gOX = (short)((b.left + b.right - APP_W) / 2);
    SetRect(&rTop, gOX, 22, gOX + APP_W, 56);
    SetRect(&rCanvas, gOX + 8, 62, gOX + 8 + CW, 62 + CW);
    rFrame = rCanvas;
    InsetRect(&rFrame, -2, -2);
    SetRect(&rPanel, gOX + 276, 60, gOX + APP_W - 4, 320);
    SetRect(&rMsg, rPanel.left + 6, 196, rPanel.right - 4, 250);
    SetRect(&rB1, rPanel.left + 6, 258, rPanel.left + 80, 282);
    SetRect(&rB2, rPanel.left + 88, 258, rPanel.right - 6, 282);
    SetRect(&rB3, rPanel.left + 6, 290, rPanel.right - 6, 316);
    SetRect(&rEndBtn, gOX + APP_W - 130, 24, gOX + APP_W - 8, 46);
    SetRect(&rStatus, gOX, 324, gOX + APP_W, 340);
    dbg.canvas = rCanvas;
}

static void SetStatus(const char *s)
{
    unsigned char p[128];
    PStr(p, s);
    EraseRect(&rStatus);
    Font(3, 9, 0);
    MoveTo(rStatus.left + 4, rStatus.bottom - 4);
    DrawString(p);
    Font(0, 12, 0);
}

static void DrawClock(void)
{
    Rect r, bar;
    long left = gState == S_DRAW ? (long)(tDeadline - TICKS) : gState == S_CARD ? ROUND_TICKS : tLeft;
    unsigned char s[16];
    short secs;
    if (left < 0) left = 0;
    if (left > ROUND_TICKS) left = ROUND_TICKS;
    secs = (short)((left * 20 + ROUND_TICKS - 1) / ROUND_TICKS);
    SetRect(&r, rTop.right - 120, rTop.top + 4, rTop.right - 4, rTop.bottom - 4);
    EraseRect(&r);
    if (gState < S_CARD || gState > S_RESULT) return;
    SetRect(&bar, r.left, r.top + 14, r.right, r.top + 24);
    FrameRect(&bar);
    InsetRect(&bar, 1, 1);
    bar.right = bar.left + (short)((long)(bar.right - bar.left) * left / ROUND_TICKS);
    FillRect(&bar, secs <= 5 ? &qd.black : &qd.gray);
    s[0] = 0;
    PCatC(s, "0:");
    s[++s[0]] = (unsigned char)('0' + secs / 10);
    s[++s[0]] = (unsigned char)('0' + secs % 10);
    Font(0, 12, 0);
    MoveTo(r.right - StringWidth(s), r.top + 11);
    DrawString(s);
}

static void DrawTop(void)
{
    unsigned char s[96], n[8];
    EraseRect(&rTop);
    Font(0, 12, 0);
    if (gState >= S_CARD && gState <= S_RESULT) {
        PStr(s, "Round ");
        NumToString(gRound + 1, n);
        PCat(s, n);
        PCatC(s, " of 6");
        MoveTo(rTop.left + 8, rTop.top + 15);
        DrawString(s);
        {
            unsigned char nm[64];
            PStr(s, "Draw ");
            ArtName(rounds[gRound].target, nm);
            PCat(s, nm);
        }
        Font(0, 12, bold);
        MoveTo(rTop.left + 8, rTop.top + 31);
        DrawString(s);
        Font(0, 12, 0);
    } else {
        Font(0, 24, bold);
        PStr(s, "Guess My Doodle");
        MoveTo(rTop.left + 8, rTop.top + 26);
        DrawString(s);
        Font(0, 12, 0);
    }
    DrawClock();
}

static void DrawStrokes(const unsigned char *x, const unsigned char *y, const unsigned char *up, short n,
                        short ox, short oy, short num, short den, short pen)
{
    short i;
    PenSize(pen, pen);
    for (i = 0; i < n; i++) {
        short a = (short)(ox + x[i] * num / den), b = (short)(oy + y[i] * num / den);
        if (i == 0 || up[i - 1]) MoveTo(a, b);
        LineTo(a, b);
    }
    PenSize(1, 1);
}

static void DrawCanvas(void)
{
    RgnHandle old = NewRgn();
    GetClip(old);
    PenSize(2, 2);
    FrameRect(&rFrame);
    PenSize(1, 1);
    EraseRect(&rCanvas);
    ClipRect(&rCanvas);
    if (gState == S_DRAW || gState == S_RESULT)
        DrawStrokes(px, py, pup, npts, rCanvas.left, rCanvas.top, 1, 1, 2);
    SetClip(old);
    DisposeRgn(old);
}

static void DrawGuesses(void)
{
    Rect r;
    short i;
    unsigned char s[96], n[8];
    SetRect(&r, rPanel.left, rPanel.top, rPanel.right, rMsg.top - 2);
    EraseRect(&r);
    Font(0, 12, 0);
    MoveTo(r.left + 6, r.top + 16);
    DrawString("\pI see\311");
    if (gTop[0] < 0) {
        Font(0, 12, 0);
        return;
    }
    ArtName(gTop[0], s);
    if (gPct[0] < 15) PStr(s, "Hmm\311");
    else PCatC(s, gState == S_RESULT && rounds[gRound].ok ? "!" : "?");
    Font(0, 12, bold);
    MoveTo(r.left + 14, r.top + 36);
    DrawString(s);
    Font(3, 9, 0);
    for (i = 0; i < 3; i++) {
        Rect bar;
        unsigned char art[16], nm[64];
        short y = (short)(r.top + 62 + i * 26);
        if (gTop[i] < 0) break;
        CatName(gTop[i], art, nm);
        MoveTo(r.left + 6, y);
        DrawString(nm);
        SetRect(&bar, r.left + 6, y + 3, r.left + 6 + 120, y + 11);
        FrameRect(&bar);
        InsetRect(&bar, 1, 1);
        bar.right = bar.left + (short)(118L * gPct[i] / 100);
        FillRect(&bar, &qd.dkGray);
        NumToString(gPct[i], n);
        PCatC(n, "%");
        MoveTo(r.left + 132, y + 10);
        DrawString(n);
    }
    Font(0, 12, 0);
}

static void Message(const unsigned char *s)
{
    Font(0, 12, 0);
    Wrap(s, &rMsg);
}

static void DrawButtons(void)
{
    if (gState == S_DRAW) {
        DrawBtn(&rB1, "Undo", npts > 0);
        DrawBtn(&rB2, "Clear", npts > 0);
        DrawBtn(&rB3, "Skip", 1);
    } else {
        EraseRect(&rB1);
        EraseRect(&rB2);
        DrawBtn(&rB3, gState == S_TITLE ? "Start" : gState == S_CARD ? "Go!" :
                     gState == S_RESULT ? (gRound + 1 < NROUNDS ? "Next" : "See results") : "Play again", 1);
    }
}

static void DrawCard(void)
{
    unsigned char s[96], n[8];
    short cx = (short)((rCanvas.left + rCanvas.right) / 2);
    EraseRect(&rCanvas);
    Font(0, 12, 0);
    PStr(s, "Round ");
    NumToString(gRound + 1, n);
    PCat(s, n);
    PCatC(s, " of 6");
    Center(s, cx, rCanvas.top + 50);
    Center("\pDraw", cx, rCanvas.top + 90);
    ArtName(rounds[gRound].target, s);
    Font(0, 24, bold);
    Center(s, cx, rCanvas.top + 128);
    Font(0, 12, 0);
    Center("\pin under 20 seconds", cx, rCanvas.top + 158);
    Font(3, 9, 0);
    Center("\pClick here or press Return when you're ready.", cx, rCanvas.top + 210);
    Font(0, 12, 0);
}

static void DrawTitle(void)
{
    static const char *const lines[] = {
        "Draw something with the mouse,",
        "and the Mac guesses what it is.",
        "",
        "The Mac names a thing; you have",
        "20 seconds to draw it. While you",
        "draw, it keeps guessing. Can it",
        "get all six?",
    };
    unsigned char s[64];
    short i;
    EraseRect(&rCanvas);
    Font(0, 12, 0);
    for (i = 0; i < (short)(sizeof lines / sizeof lines[0]); i++) {
        PStr(s, lines[i]);
        MoveTo(rCanvas.left + 16, rCanvas.top + 40 + i * 18);
        DrawString(s);
    }
    Font(3, 9, 0);
    MoveTo(rCanvas.left + 16, rCanvas.bottom - 16);
    DrawString("\pPress Return or click Start.");
    {                                               /* a sample: a little house and the Mac's guess */
        static const unsigned char hx[] = {50, 50, 206, 206, 50, 40, 128, 216, 110, 110, 146, 146};
        static const unsigned char hy[] = {120, 230, 230, 120, 120, 126, 40, 126, 230, 175, 175, 230};
        static const unsigned char hu[] = {0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1};
        Rect f;
        SetRect(&f, rPanel.left + 30, rPanel.top + 20, rPanel.left + 30 + 100, rPanel.top + 20 + 100);
        FrameRect(&f);
        DrawStrokes(hx, hy, hu, 12, f.left + 2, f.top + 2, 3, 8, 1);
        Font(0, 12, 0);
        MoveTo(rPanel.left + 30, f.bottom + 22);
        DrawString("\pI see\311");
        Font(0, 12, bold);
        MoveTo(rPanel.left + 38, f.bottom + 40);
        DrawString("\pa house!");
    }
    Font(0, 12, 0);
}

static void DrawEnd(void)
{
    Rect all, f;
    short r, i;
    unsigned char s[96], n[8];
    unsigned long t0 = TICKS;
    SetRect(&all, gOX, 20, gOX + APP_W, 342);
    EraseRect(&all);
    Font(0, 12, bold);
    PStr(s, "The Mac guessed ");
    NumToString(gScore, n);
    PCat(s, n);
    PCatC(s, " of 6!");
    MoveTo(gOX + 8, 40);
    DrawString(s);
    DrawBtn(&rEndBtn, "Play again", 1);
    dbg.endTicks[0] = TICKS - t0;
    for (r = 0; r < NROUNDS; r++) {                     /* 3 x 2 cells of 148 x 136 */
        short x0 = (short)(gOX + 4 + (r % 3) * 148), y0 = (short)(52 + (r / 3) * 136);
        unsigned char art[16], nm[64];
        SetRect(&f, x0 + 2, y0, x0 + 92, y0 + 90);
        FrameRect(&f);
        DrawStrokes(sx[r], sy[r], sup[r], rounds[r].n, f.left + 1, f.top + 1, 11, 32, 1);
        if (r == 0) dbg.endTicks[1] = TICKS - t0;
        CatName(rounds[r].target, art, nm);
        Font(0, 12, rounds[r].ok ? 0 : bold);
        MoveTo(f.right + 6, f.top + 14);
        DrawChar(rounds[r].ok ? 0x12 : 'X');            /* Chicago's check mark */
        Font(3, 9, bold);
        MoveTo(x0 + 2, f.bottom + 12);
        DrawString(nm);
        Font(3, 9, 0);
        for (i = 0; i < 3; i++) {
            if (rounds[r].top[i] < 0) break;
            CatName(rounds[r].top[i], art, nm);
            s[0] = 0;
            s[++s[0]] = (unsigned char)('1' + i);
            PCatC(s, ". ");
            PCat(s, nm);
            NumToString(rounds[r].pct[i], n);
            PCatC(s, " ");
            PCat(s, n);
            PCatC(s, "%");
            MoveTo(x0 + 8, f.bottom + 23 + i * 10);
            DrawString(s);
        }
    }
    dbg.endTicks[2] = (unsigned long)rounds[0].n;
    dbg.endTicks[3] = TICKS - t0;
    Font(0, 12, 0);
}

static void DrawAll(void)
{
    Rect all;
    SetRect(&all, gOX, 20, gOX + APP_W, 342);
    EraseRect(&all);
    if (gState == S_END) {
        DrawEnd();
        SetStatus("Press Return to play again.");
        return;
    }
    DrawTop();
    if (gState == S_TITLE || gState == S_START || gState == S_ERROR) {
        PenSize(2, 2);
        FrameRect(&rFrame);
        PenSize(1, 1);
        if (gState == S_TITLE) {
            DrawTitle();
            DrawMenuBar();          /* (in case the cursor sat on the menu bar when it was first drawn) */
        }
    } else if (gState == S_CARD) {
        PenSize(2, 2);
        FrameRect(&rFrame);
        PenSize(1, 1);
        DrawCard();
    } else {
        DrawCanvas();
    }
    if (gState == S_DRAW || gState == S_RESULT) DrawGuesses();
    DrawButtons();
    if (gState == S_DRAW) SetStatus("Draw in the box. Undo: Command-Z.  Sound on/off: M.  Quit: Esc.");
    else if (gState != S_ERROR) SetStatus("A neural network trained on Quick, Draw! (Google, CC BY 4.0).");
}

/* ------------------------------------------------------------------ guessing */
static void Softmax(const short *lo, short n)
{
    short i, j, mx = lo[0];
    unsigned long sum = 0;
    for (i = 1; i < n; i++) if (lo[i] > mx) mx = lo[i];
    for (i = 0; i < n; i++) {
        short d = mx - lo[i];
        sum += d < 512 ? expt[d] : 0;
    }
    for (j = 0; j < 3; j++) {
        short best = -1;
        for (i = 0; i < n; i++) {
            if (j > 0 && (i == gTop[0] || (j > 1 && i == gTop[1]))) continue;
            if (best < 0 || lo[i] > lo[best]) best = i;
        }
        gTop[j] = best;
        {
            short d = mx - lo[best];
            unsigned long e = d < 512 ? expt[d] : 0;
            gPct[j] = (short)((e * 100 + sum / 2) / sum);
        }
        dbg.top[j] = gTop[j];
        dbg.pct[j] = gPct[j];
    }
    dbg.top1 = gTop[0];
    dbg.top1pct = gPct[0];
}

static void StartGuess(void)
{
    unsigned char bits[128];
    rasterize(px, py, pup, npts, bits);
    g_begin(&M, bits);
    inferring = 1;
    inferVer = drawVer;
    tInfer = TICKS;
}

static void SpeakNow(const unsigned char *s)
{
    unsigned long t0 = TICKS;
    if (!gSp.on || gSp.state != 1) return;
    snd_hold(1);
    spSay(&gSp, (const char *)s + 1, s[0]);
    dbg.spoken = gSp.count;
    dbg.spokenErr = gSp.err;
    snd_hold(0);            /* (the mouse samples taken meanwhile are drawn when the main loop reads them) */
    tDeadline += TICKS - t0;
    tLastSpeak = TICKS;
}

static void EndRound(short ok);

static void GuessDone(void)
{
    short prev = gTop[0];
    inferring = 0;
    dbg.inferTicks = TICKS - tInfer;
    Softmax(g_logits(&M), M.ncls);
    dbg.ninfer++;
    if (gState != S_DRAW) return;
    if (!npts) {                                /* cleared (or undone) while it was guessing */
        gTop[0] = -1;
        DrawGuesses();
        return;
    }
    DrawGuesses();
    if (gTop[0] == rounds[gRound].target) {
        EndRound(1);
        return;
    }
    if (gTop[0] != prev && gPct[0] >= 15) {
        snd_play(SND_BLIP);
        if (!penDown && gTop[0] != gSaid && gPct[0] >= 25 && TICKS - tLastSpeak > 240 && gSp.on) {
            unsigned char s[96], a[64];
            PStr(s, "Is it ");
            ArtName(gTop[0], a);
            PCat(s, a);
            PCatC(s, "?");
            gSaid = gTop[0];
            SpeakNow(s);
        }
    }
}

/* finish the guess in progress and make one of the final drawing (at the end of the clock) */
static void FinalGuess(void)
{
    if (!npts) return;
    if (inferring) while (!g_step(&M)) { }
    if (inferring) GuessDone();
    if (gState == S_DRAW && inferVer != drawVer) {
        StartGuess();
        while (!g_step(&M)) { }
        GuessDone();
    }
}

/* ------------------------------------------------------------------ the drawing */
static void Segment(short x0, short y0, short x1, short y1)
{
    RgnHandle old = NewRgn();
    GetClip(old);
    ClipRect(&rCanvas);
    PenSize(2, 2);
    MoveTo(rCanvas.left + x0, rCanvas.top + y0);
    LineTo(rCanvas.left + x1, rCanvas.top + y1);
    PenSize(1, 1);
    SetClip(old);
    DisposeRgn(old);
}

static void AddPoint(short x, short y)
{
    if (npts >= MAXPTS) return;
    px[npts] = (unsigned char)x;
    py[npts] = (unsigned char)y;
    pup[npts] = 0;
    npts++;
    drawVer++;
    dbg.npts = npts;
}

static short Clamp(short v) { return v < 0 ? 0 : v > CW - 1 ? CW - 1 : v; }

static void InkSample(short v, short h, short down)
{
    short x = h - rCanvas.left, y = v - rCanvas.top;
    if (gState != S_DRAW) { penDown = 0; return; }
    if (down && !penDown) {
        if (x < 0 || x >= CW || y < 0 || y >= CW || npts >= MAXPTS - 1) return;
        penDown = 1;
        AddPoint(x, y);
        Segment(x, y, x, y);
        lastX = x; lastY = y;
        if (npts == 1) DrawButtons();
    } else if (penDown) {
        x = Clamp(x); y = Clamp(y);
        if (x - lastX >= MINSTEP || lastX - x >= MINSTEP || y - lastY >= MINSTEP || lastY - y >= MINSTEP
            || (!down && (x != lastX || y != lastY))) {
            AddPoint(x, y);
            Segment(lastX, lastY, x, y);
            lastX = x; lastY = y;
        }
        if (!down) {
            pup[npts - 1] = 1;
            penDown = 0;
            drawVer++;
        }
    }
}

static void Undo(void)
{
    if (gState != S_DRAW || penDown || !npts) return;
    npts--;                                             /* the last stroke: back to the previous lift */
    while (npts > 0 && !pup[npts - 1]) npts--;
    drawVer++;
    dbg.npts = npts;
    DrawCanvas();
    DrawButtons();
    if (!npts) { gTop[0] = -1; DrawGuesses(); }
}

static void ClearDrawing(void)
{
    if (gState != S_DRAW || penDown) return;
    npts = 0;
    drawVer++;
    dbg.npts = 0;
    DrawCanvas();
    DrawButtons();
    gTop[0] = -1;
    DrawGuesses();
}

/* ------------------------------------------------------------------ game flow */
static void SetState(short s)
{
    gState = s;
    dbg.state = (unsigned long)s;
}

static void NewGame(void)
{
    short r, j;
    seed = TICKS | 1;
    for (r = 0; r < NROUNDS; r++) {
        short t;
        do {
            t = (short)(rnd() % (unsigned long)M.ncls);
            for (j = 0; j < r && rounds[j].target != t; j++) { }
        } while (j < r);
        rounds[r].target = t;
        rounds[r].n = 0;
        rounds[r].ok = 0;
        rounds[r].top[0] = -1;
    }
    if (gForce >= 0 && gForce < M.ncls) {
        for (r = 1; r < NROUNDS; r++) if (rounds[r].target == gForce) rounds[r].target = rounds[0].target;
        rounds[0].target = gForce;
        gForce = -1;
    }
    gRound = 0;
    gScore = 0;
    dbg.score = 0;
    dbg.okMask = 0;
    dbg.round = 0;
    dbg.target = rounds[0].target;
    SetState(S_CARD);
    DrawAll();
}

static void StartRound(void)
{
    npts = 0;
    penDown = 0;
    drawVer = inferVer = 0;
    inferring = 0;
    gTop[0] = gTop[1] = gTop[2] = -1;
    gSaid = -1;
    gTicked = 6;
    dbg.npts = 0;
    dbg.top1 = -1;
    ink_flush();
    SetState(S_DRAW);
    DrawAll();
    snd_play(SND_START);
    tDeadline = TICKS + ROUND_TICKS;
    tLastSpeak = 0;
}

static void EndRound(short ok)
{
    Round *r = &rounds[gRound];
    unsigned char s[128], a[64];
    short i;
    inferring = 0;
    penDown = 0;
    tLeft = (long)(tDeadline - TICKS);
    if (tLeft < 0) tLeft = 0;
    if (npts > 0 && !pup[npts - 1]) pup[npts - 1] = 1;
    r->ok = ok;
    r->n = npts;
    r->secs = (short)((ROUND_TICKS - (long)(tDeadline - TICKS)) / 60);
    for (i = 0; i < 3; i++) { r->top[i] = npts ? gTop[i] : -1; r->pct[i] = gPct[i]; }
    BlockMove(px, sx[gRound], npts);
    BlockMove(py, sy[gRound], npts);
    BlockMove(pup, sup[gRound], npts);
    if (ok) {
        gScore++;
        dbg.score = gScore;
        dbg.okMask |= 1L << gRound;
    }
    SetState(S_RESULT);
    DrawClock();
    DrawGuesses();
    DrawButtons();
    ArtName(r->target, a);
    if (ok) {
        PStr(s, "Oh, I know! It's ");
        PCat(s, a);
        PCatC(s, "!");
    } else if (npts && gTop[0] >= 0) {
        unsigned char g[64];
        ArtName(gTop[0], g);
        PStr(s, "Sorry, I couldn't guess it. I thought it was ");
        PCat(s, g);
        PCatC(s, ".");
    } else {
        PStr(s, "Sorry, I couldn't guess it.");
    }
    Message(s);
    snd_play(ok ? SND_CHIME : SND_FAIL);
    tResult = TICKS;
    if (gSp.on && gSp.state == 1) {
        while (snd_busy() && TICKS - tResult < 90) { }
        if (ok) { PStr(s, "Oh, I know! It's "); PCat(s, a); PCatC(s, "!"); }
        else PStr(s, "Sorry, I couldn't guess it.");
        SpeakNow(s);
    }
    tResult = TICKS;
}

static void NextRound(void)
{
    if (gRound + 1 < NROUNDS) {
        gRound++;
        dbg.round = gRound;
        dbg.target = rounds[gRound].target;
        SetState(S_CARD);
        DrawAll();
    } else {
        SetState(S_END);
        DrawAll();
        snd_play(SND_FANFARE);
        if (gSp.on && gSp.state == 1) {
            unsigned char s[64], n[8];
            PStr(s, "I guessed ");
            NumToString(gScore, n);
            PCat(s, n);
            PCatC(s, " out of six.");
            while (snd_busy()) { }
            SpeakNow(s);
        }
    }
}

static void Primary(void)       /* Return, the big button, a click on the card */
{
    switch (gState) {
    case S_TITLE: NewGame(); break;
    case S_CARD: StartRound(); break;
    case S_DRAW: EndRound(0); break;            /* Skip */
    case S_RESULT: NextRound(); break;
    case S_END: NewGame(); break;
    }
}

static void ToTitle(void)
{
    inferring = 0;
    SetState(S_TITLE);
    DrawAll();
}

/* ------------------------------------------------------------------ menus, About */
static void SetupMenus(void)
{
    mApple = NewMenu(M_APPLE, "\p\x14");
    AppendMenu(mApple, "\pAbout Guess My Doodle\311;(-");
    AppendResMenu(mApple, 'DRVR');
    InsertMenu(mApple, 0);
    mFile = NewMenu(M_FILE, "\pFile");
    AppendMenu(mFile, "\pNew Game/N;(-;Sound;Speak Guesses;(-;Quit/Q");
    InsertMenu(mFile, 0);
    mEdit = NewMenu(M_EDIT, "\pEdit");
    AppendMenu(mEdit, "\pUndo/Z;(-;Cut/X;Copy/C;Paste/V;Clear;(-;Clear Drawing/K");
    InsertMenu(mEdit, 0);
    DrawMenuBar();
}

static void UpdateMenus(void)
{
    CheckItem(mFile, I_SOUND, gSoundOn);
    CheckItem(mFile, I_SPEAK, gSp.state == 1 && gSp.on);
    if (gSp.state == 1) EnableItem(mFile, I_SPEAK);
    else DisableItem(mFile, I_SPEAK);
}

static void About(void)
{
    WindowPtr w;
    Rect r;
    EventRecord ev;
    static const char *const lines[] = {
        "Guess My Doodle",
        "Draw something and the Mac guesses what it is.",
        "",
        "The guesses come from a small neural network (a",
        "convolutional classifier with weights of -1, 0 and +1,",
        "turned into 68000 code when the game starts), trained",
        "on Google's Quick, Draw! dataset (CC BY 4.0).",
        "It speaks its guesses when MacinTalk is installed.",
        "",
        "Click or press a key to continue.",
    };
    unsigned char s[96];
    short i, n = sizeof lines / sizeof lines[0];
    SetRect(&r, gOX + 40, 70, gOX + APP_W - 40, 70 + 24 + n * 13);
    w = NewWindow(NULL, &r, "\p", true, dBoxProc, (WindowPtr)-1, false, 0);
    SetPort(w);
    for (i = 0; i < n; i++) {
        Font(i == 0 ? 0 : 3, i == 0 ? 12 : 9, 0);
        PStr(s, lines[i]);
        MoveTo(14, 20 + i * 13);
        DrawString(s);
    }
    for (;;) {
        SystemTask();
        if (GetNextEvent(mDownMask | keyDownMask, &ev)) break;
    }
    DisposeWindow(w);
    SetPort(gWin);
    Font(0, 12, 0);
    ink_flush();
}

static void Quit(void)
{
    snd_quit();
    ink_quit();
    ExitToShell();
}

static void DoMenu(long mr)
{
    short menu = (short)(mr >> 16), item = (short)mr;
    switch (menu) {
    case M_APPLE:
        if (item == 1) {
            About();
        } else {
            Str255 name;
            GrafPtr old;
            GetPort(&old);
            GetMenuItemText(mApple, item, name);
            OpenDeskAcc(name);
            SetPort(old);
        }
        break;
    case M_FILE:
        if (item == I_NEW && gState != S_START && gState != S_ERROR) NewGame();
        else if (item == I_SOUND) gSoundOn = !gSoundOn;
        else if (item == I_SPEAK && gSp.state == 1) gSp.on = spPref(&gSp, !gSp.on);
        else if (item == I_QUIT) Quit();
        UpdateMenus();
        break;
    case M_EDIT:
        if (item <= I_CLEAR && SystemEdit(item - 1)) break;
        if (item == I_UNDO) Undo();
        else if (item == I_CLEARALL) ClearDrawing();
        break;
    }
    HiliteMenu(0);
}

/* ------------------------------------------------------------------ start-up */
static OSErr HOpenRead(ConstStr255Param name, short vref, long dir, short *ref)
{
    HParamBlockRec pb;
    OSErr err;
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioNamePtr = (StringPtr)name;
    pb.ioParam.ioVRefNum = vref;
    pb.ioParam.ioPermssn = fsRdPerm;
    pb.fileParam.ioDirID = dir;
    err = PBHOpenSync(&pb);
    *ref = pb.ioParam.ioRefNum;
    return err;
}

/* "Guess Model": in the application's folder, else the default folder, else the System Folder */
static OSErr OpenModel(short *ref, short *appVol)
{
    static const unsigned char name[] = "\pGuess Model";
    FCBPBRec fcb;
    SysEnvRec env;
    short appRef;
    __asm__ volatile("move.w 0x900.w,%0" : "=d"(appRef));     /* CurApRefNum */
    memset(&fcb, 0, sizeof fcb);
    fcb.ioRefNum = appRef;
    *appVol = 0;
    if (PBGetFCBInfoSync(&fcb) == noErr) {
        *appVol = fcb.ioFCBVRefNum;
        if (HOpenRead(name, fcb.ioFCBVRefNum, fcb.ioFCBParID, ref) == noErr) return noErr;
    }
    if (FSOpen(name, 0, ref) == noErr) return noErr;
    SysEnvirons(1, &env);
    return FSOpen(name, env.sysVRefNum, ref);
}

static void *zalloc(long n) { return NewPtrClear(n); }
static void zfree(void *p) { DisposePtr((Ptr)p); }

static int LoadModel(void)
{
    short ref, vol, i;
    long len;
    unsigned long t0 = TICKS;
    int e;
    if (OpenModel(&ref, &vol) != noErr) { dbg.modelErr = 10; return 0; }
    GetEOF(ref, &len);
    gFile = NewPtr(len);
    if (!gFile) { FSClose(ref); dbg.modelErr = 11; return 0; }
    if (FSRead(ref, &len, gFile) != noErr) { FSClose(ref); dbg.modelErr = 12; return 0; }
    FSClose(ref);
    SetStatus("Building the network\311");
    e = g_init(&M, (const unsigned char *)gFile, len, zalloc, zfree);
    dbg.modelErr = e;
    dbg.loadTicks = (long)(TICKS - t0);
    dbg.ncls = M.ncls;
    dbg.codeBytes = (unsigned long)M.codebytes;
    if (e) return 0;
    {
        unsigned long v = 65535;                         /* exp(-d / 32), d = 0..511 */
        for (i = 0; i < 512; i++) {
            expt[i] = (unsigned short)v;
            v = (v * 63519UL) >> 16;
        }
    }
    spInit(&gSp, vol, 1);
    gSp.on = gSp.state == 1 ? spPref(&gSp, -1) : 0;
    dbg.speech = gSp.state;
    return 1;
}

/* ------------------------------------------------------------------ main */
static void Mailbox(void)
{
    unsigned long c = dbg.cmd;
    if (!c) return;
    dbg.cmd = 0;
    if (c == 1) gForce = (short)dbg.arg;
    dbg.cmdDone++;
}

static void Click(EventRecord *ev)
{
    Point p = ev->where;
    if (gState == S_DRAW) {
        if (PtInRect(p, &rB1) && npts && TrackButton(&rB1)) Undo();
        else if (PtInRect(p, &rB2) && npts && TrackButton(&rB2)) ClearDrawing();
        else if (PtInRect(p, &rB3) && TrackButton(&rB3)) Primary();
        return;
    }
    if (gState == S_END) {
        if (PtInRect(p, &rEndBtn) && TrackButton(&rEndBtn)) Primary();
    } else if (PtInRect(p, &rB3)) {
        if (TrackButton(&rB3)) Primary();
    } else if (gState == S_CARD && PtInRect(p, &rCanvas)) {
        while (StillDown()) { }
        Primary();
    } else if (gState == S_RESULT) {
        Primary();
    }
}

static void Key(EventRecord *ev)
{
    char c = (char)(ev->message & charCodeMask);
    short code = (short)((ev->message & keyCodeMask) >> 8);
    if (ev->modifiers & cmdKey) {
        if (ev->what == keyDown) DoMenu(MenuKey(c));
        return;
    }
    if (code == 0x32 || code == 0x35 || c == 0x1B) {            /* Esc (the Plus's ` key, the SE's Esc) */
        if (gState == S_TITLE || gState == S_ERROR) Quit();
        else ToTitle();
    } else if (c == '\r' || c == 3) {
        if (gState != S_DRAW) Primary();
    } else if (c == 'm' || c == 'M') {
        gSoundOn = !gSoundOn;
        UpdateMenus();
    } else if (c == 8) {
        Undo();                                                 /* Delete */
    }
}

int main(void)
{
    EventRecord ev;
    WindowPtr w;
    Rect r;
    short part, v, h, down;

    dbg.magic = 0x47534442UL;               /* 'GSDB' */
    dbg.version = 1;
    dbg.notMagic = ~0x47534442UL;
    dbg.top1 = -1;
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
    SetState(S_START);
    DrawAll();
    SetStatus("Loading the brain\311");
    if (!LoadModel()) {
        SetState(S_ERROR);
        DrawAll();
        Message(dbg.modelErr == 2 ? "\pNot enough memory for the network. Press Esc to quit."
                                  : "\pThe file \"Guess Model\" is missing or damaged. Press Esc to quit.");
        SetStatus("Guess My Doodle needs the file \"Guess Model\" next to it.");
    } else {
        dbg.freeMem = FreeMem();
        snd_init();
        ink_init();
        UpdateMenus();
        SetState(S_TITLE);
        DrawAll();
    }

    for (;;) {
        SystemTask();
        Mailbox();
        while (ink_next(&v, &h, &down)) InkSample(v, h, down);
        if (gState == S_DRAW) {
            long left = (long)(tDeadline - TICKS);
            if (inferring) {
                unsigned long t0 = TICKS;          /* steps back to back while no mouse samples wait */
                int done;
                while (!(done = g_step(&M)) && TICKS - t0 < 4 && !ink_pending()) { }
                if (done) GuessDone();
            } else if (drawVer != inferVer && npts && (!penDown || TICKS - tInfer > 75)) {
                StartGuess();
            }
            if (gState == S_DRAW) {
                left = (long)(tDeadline - TICKS);
                if (left <= 0) {
                    FinalGuess();
                    if (gState == S_DRAW) EndRound(0);
                } else {
                    short secs = (short)((left * 20 + ROUND_TICKS - 1) / ROUND_TICKS);
                    static short shown = -1;
                    if (secs != shown) { shown = secs; DrawClock(); }
                    if (secs <= 5 && secs < gTicked) { gTicked = secs; snd_play(SND_TICK); }
                }
            }
        } else if (gState == S_RESULT && TICKS - tResult > RESULT_TICKS) {
            NextRound();
        }
        if (!GetNextEvent(everyEvent, &ev)) continue;
        switch (ev.what) {
        case mouseDown:
            part = FindWindow(ev.where, &w);
            if (part == inMenuBar) {
                UpdateMenus();
                DoMenu(MenuSelect(ev.where));
                ink_flush();
            } else if (part == inSysWindow) {
                SystemClick(&ev, w);
            } else if (w == gWin) {
                SetPort(gWin);
                Click(&ev);
            }
            break;
        case keyDown:
        case autoKey:
            Key(&ev);
            break;
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

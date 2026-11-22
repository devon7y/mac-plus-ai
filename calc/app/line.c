/*
 * Scribble Calc: the line being written. Pen and eraser tracking, undo, recognition scheduling
 * (symbols are classified in idle time), grammar decoding, the exact answer and the history tape.
 */
#include "calc.h"
#include "net_gen.h"

TapeEntry gTape[TAPE_N];
short gTapeN, gTapeCur = -1;
signed char gTok[MAXTOK];
short gNTok, gEqAt = -1, gErr = -1;
char gAnsText[DEC_BUFLEN];
short gAnsSci, gAnsExp;
unsigned long gLastInkTick;

static Dec gAns;
static Boolean gNeedDecode, gPaperDirty, gTapeDirty;
static short BuildTokens(signed char *out, short upto, short max);
typedef struct {
    short ok, eq, x1, href, sci, exp;
    char text[DEC_BUFLEN];
} Drawn;
static Drawn gLastDrawn;                       /* the answer last rendered on the paper */

/* ------------------------------------------------------------------ undo */
enum { U_ADD = 1, U_ERASE, U_CLEAR, U_FIX };
typedef struct {
    unsigned char type;
    signed char oldForce;
    unsigned char oldN;
    unsigned char pad;
    short a, b;
    short tapeCur;
} Undo;
#define MAXUNDO 80
#define MAXUL 1600
static Undo gUndo[MAXUNDO];
static short gNUndo;
static short gUL[MAXUL];                       /* stroke lists of erase and clear records */
static short gNUL;

static void ForgetUndo(void)
{
    gNUndo = 0;
    gNUL = 0;
}

static Undo *PushUndo(unsigned char type)
{
    if (gNUndo == MAXUNDO) {                    /* drop the oldest record */
        short i, drop = 0;
        if (gUndo[0].type == U_ERASE || gUndo[0].type == U_CLEAR) {
            drop = gUndo[0].b;
            for (i = 0; i + drop < gNUL; i++)
                gUL[i] = gUL[i + drop];
            gNUL = (short)(gNUL - drop);
        }
        for (i = 1; i < gNUndo; i++) {
            gUndo[i - 1] = gUndo[i];
            if (gUndo[i - 1].type == U_ERASE || gUndo[i - 1].type == U_CLEAR)
                gUndo[i - 1].a = (short)(gUndo[i - 1].a - drop);
        }
        gNUndo--;
    }
    gUndo[gNUndo].type = type;
    return &gUndo[gNUndo++];
}

/* Drop dead strokes and their points (when the arrays fill up); undo history is lost. */
static void Compact(void)
{
    short i, ns = 0, np = 0, j;
    for (i = 0; i < gNStk; i++) {
        Stroke s = gStk[i];
        if (!s.alive)
            continue;
        for (j = 0; j < s.n; j++)
            gPts[np + j] = gPts[s.first + j];
        s.first = np;
        np = (short)(np + s.n);
        s.cached = 0;
        gStk[ns++] = s;
    }
    gNStk = ns;
    gNPts = np;
    ForgetUndo();
}

/* ------------------------------------------------------------------ pen */
static Pt ToPaper(Point m)
{
    Pt p;
    short w = (short)(gPaperR.right - gPaperR.left - 1), h = (short)(gPaperR.bottom - gPaperR.top - 1);
    p.x = (short)(m.h - gPaperR.left);
    p.y = (short)(m.v - gPaperR.top);
    if (p.x < 0) p.x = 0;
    if (p.y < 0) p.y = 0;
    if (p.x > w) p.x = w;
    if (p.y > h) p.y = h;
    return p;
}

void TrackPen(Point where)
{
    Stroke *s;
    Pt p, last;
    Point m;
    if (gNStk >= MAXSTK || gNPts >= MAXPTS - 64)
        Compact();
    if (gNStk >= MAXSTK || gNPts >= MAXPTS - 64) {
        SysBeep(10);
        SetStatus("The paper is full: press Return for a new line.");
        return;
    }
    s = &gStk[gNStk];
    s->first = gNPts;
    s->n = 0;
    last = p = ToPaper(where);
    gPts[gNPts++] = p;
    s->n = 1;
    PaperStrokeSegment(p, p);
    while (StillDown()) {
        GetMouse(&m);
        p = ToPaper(m);
        if (p.x == last.x && p.y == last.y)
            continue;
        if (gNPts >= MAXPTS - 1)
            break;
        gPts[gNPts++] = p;
        s->n++;
        PaperStrokeSegment(last, p);
        last = p;
    }
    {
        short i;
        const Pt *q = gPts + s->first;
        s->x0 = s->x1 = q[0].x;
        s->y0 = s->y1 = q[0].y;
        for (i = 1; i < s->n; i++) {
            if (q[i].x < s->x0) s->x0 = q[i].x;
            if (q[i].x > s->x1) s->x1 = q[i].x;
            if (q[i].y < s->y0) s->y0 = q[i].y;
            if (q[i].y > s->y1) s->y1 = q[i].y;
        }
    }
    s->alive = 1;
    s->force = NOFORCE;
    s->forceN = 0;
    s->cached = 0;
    PushUndo(U_ADD)->a = gNStk;
    gNStk++;
    gLastInkTick = TickCount();
    LineChanged();
}

/* ------------------------------------------------------------------ eraser */
#define ER 5

static Boolean NearStroke(const Stroke *s, short px, short py)
{
    const Pt *q = gPts + s->first;
    short i;
    if (px < s->x0 - ER || px > s->x1 + ER || py < s->y0 - ER || py > s->y1 + ER)
        return false;
    for (i = 0; i < s->n; i++) {
        short ax = q[i].x, ay = q[i].y, bx, by, n, k, dx, dy;
        if (i + 1 < s->n) {
            bx = q[i + 1].x;
            by = q[i + 1].y;
        } else {
            bx = ax;
            by = ay;
        }
        dx = (short)(bx - ax);
        dy = (short)(by - ay);
        n = (short)(((dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy)) / 2 + 1);
        for (k = 0; k <= n; k++) {
            short x = (short)(ax + (long)dx * k / n - px), y = (short)(ay + (long)dy * k / n - py);
            if ((long)x * x + (long)y * y <= ER * ER)
                return true;
        }
    }
    return false;
}

static short EraseAt(short px, short py)
{
    short i, killed = 0;
    for (i = 0; i < gNStk; i++) {
        Stroke *s = &gStk[i];
        if (s->alive && NearStroke(s, px, py) && gNUL < MAXUL) {
            s->alive = 0;
            gUL[gNUL++] = i;
            killed++;
        }
    }
    return killed;
}

void TrackEraser(Point where)
{
    short start = gNUL, n = 0;
    Pt last = ToPaper(where), p;
    Point m;
    n = (short)(n + EraseAt(last.x, last.y));
    if (n) {
        PaperRender();
        PaperShow(NULL);
    }
    while (StillDown()) {
        short k, steps, dx, dy, got = 0;
        GetMouse(&m);
        p = ToPaper(m);
        if (p.x == last.x && p.y == last.y)
            continue;
        dx = (short)(p.x - last.x);
        dy = (short)(p.y - last.y);
        steps = (short)(((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy)) / 3 + 1);
        for (k = 1; k <= steps; k++)
            got = (short)(got + EraseAt((short)(last.x + (long)dx * k / steps), (short)(last.y + (long)dy * k / steps)));
        last = p;
        if (got) {
            n = (short)(n + got);
            PaperRender();
            PaperShow(NULL);
        }
    }
    if (n) {
        Undo *u = PushUndo(U_ERASE);
        u->a = start;
        u->b = n;
        gPaperDirty = true;
        LineChanged();
    }
}

/* ------------------------------------------------------------------ undo, clear, corrections */
void DoUndo(void)
{
    Undo u;
    short i;
    if (!gNUndo) {
        SysBeep(1);
        return;
    }
    u = gUndo[--gNUndo];
    switch (u.type) {
    case U_ADD:
        gStk[u.a].alive = 0;
        if (u.a == gNStk - 1) {                 /* give its points back */
            gNPts = gStk[u.a].first;
            gNStk--;
        }
        break;
    case U_ERASE:
    case U_CLEAR:
        for (i = 0; i < u.b; i++)
            gStk[gUL[u.a + i]].alive = 1;
        gNUL = u.a;
        if (u.type == U_CLEAR)
            gTapeCur = u.tapeCur;
        break;
    case U_FIX:
        gStk[u.a].force = u.oldForce;
        gStk[u.a].forceN = u.oldN;
        break;
    }
    gPaperDirty = true;
    LineChanged();
}

void DoClear(void)
{
    short i, n = 0, start = gNUL;
    for (i = 0; i < gNStk; i++)
        if (gStk[i].alive && gNUL < MAXUL) {
            gStk[i].alive = 0;
            gUL[gNUL++] = i;
            n++;
        }
    if (n) {
        Undo *u = PushUndo(U_CLEAR);
        u->a = start;
        u->b = n;
        u->tapeCur = gTapeCur;
    }
    gTapeCur = -1;
    gPaperDirty = true;
    LineChanged();
}

static void SetForce(short gi, short cls)
{
    Group *g = &gGrp[gi];
    Stroke *k = &gStk[g->key];
    Undo *u = PushUndo(U_FIX);
    u->a = g->key;
    u->oldForce = k->force;
    u->oldN = k->forceN;
    k->force = (signed char)cls;
    k->forceN = (unsigned char)g->n;
    LineChanged();
}

void ClickStrip(Point where)
{
    short i, best = -1, bd = 32767;
    for (i = 0; i < gNGrp; i++) {
        short cx = (short)(gPaperR.left + (gGrp[i].x0 + gGrp[i].x1) / 2), d = (short)(where.h - cx);
        if (d < 0)
            d = (short)-d;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    if (best < 0 || bd > 20 || !gGrp[best].ready) {
        SetStatus(gNGrp ? "Click the symbol you want to fix." : "Write a sum on the paper, then =.");
        return;
    }
    i = (short)ChoosePopup(best, where);
    DrawAll();
    if (i >= 0)
        SetForce(best, i);
    else if (i == -2)
        SetForce(best, NOFORCE);
}

/* ------------------------------------------------------------------ recognition and the answer */
void LineChanged(void)
{
    rec_segment();
    gNeedDecode = true;
    dbg.nGrp = gNGrp;
    dbg.href = gHref;
    {
        short i, n = 0;
        for (i = 0; i < gNStk; i++)
            n = (short)(n + gStk[i].alive);
        dbg.nStk = n;
        dbg.nStkAll = gNStk;
    }
    DrawStrip();
}

static void TapeText(const TapeEntry *e, char *out)
{
    short i = 0, k;
    for (k = 0; e->result[k]; k++)
        out[i++] = e->result[k];
    if (e->sci) {
        short v = e->exp10;
        char tmp[8];
        short n = 0;
        out[i++] = 'E';
        if (v < 0) {
            out[i++] = '-';
            v = (short)-v;
        }
        do {
            tmp[n++] = (char)('0' + v % 10);
            v = (short)(v / 10);
        } while (v);
        while (n)
            out[i++] = tmp[--n];
    }
    out[i] = 0;
}

static void PutText(const char *t)
{
    long n = 0;
    while (t[n])
        n++;
    ZeroScrap();
    PutScrap(n, 'TEXT', (Ptr)t);
}

void DoCopy(void)
{
    char buf[DEC_BUFLEN + 8];
    if (gEqAt >= 0 && gErr == DEC_OK) {
        dec_to_text(&gAns, buf);
    } else if (gTapeN && gTape[0].err == DEC_OK) {
        TapeText(&gTape[0], buf);
    } else {
        SysBeep(1);
        return;
    }
    PutText(buf);
    {
        char msg[64] = "Copied ";
        short i = 7, k;
        for (k = 0; buf[k] && i < 62; k++)
            msg[i++] = buf[k];
        msg[i] = 0;
        SetStatus(msg);
    }
}

void CopyTape(short i)
{
    char buf[DEC_BUFLEN + 8];
    if (i < 0 || i >= gTapeN || gTape[i].err != DEC_OK)
        return;
    TapeText(&gTape[i], buf);
    PutText(buf);
    {
        char msg[64] = "Copied ";
        short j = 7, k;
        for (k = 0; buf[k] && j < 62; k++)
            msg[j++] = buf[k];
        msg[j] = 0;
        SetStatus(msg);
    }
}

static void TapeRemoveCur(void)
{
    short i;
    if (gTapeCur < 0 || gTapeCur >= gTapeN)
        return;
    gTapeDirty = true;
    for (i = gTapeCur; i + 1 < gTapeN; i++)
        gTape[i] = gTape[i + 1];
    gTapeN--;
    gTapeCur = -1;
}

static void TapeSetCur(void)
{
    TapeEntry *e;
    short i, n;
    (void)i;
    if (gTapeCur < 0) {                          /* a new entry at the top */
        if (gTapeN < TAPE_N)
            gTapeN++;
        for (i = (short)(gTapeN - 1); i > 0; i--)
            gTape[i] = gTape[i - 1];
        gTapeCur = 0;
    }
    e = &gTape[gTapeCur];
    gTapeDirty = true;
    n = BuildTokens(e->tok, gEqAt, TAPE_TOK - 1);
    e->tok[n++] = T_EQ;
    e->nTok = n;
    e->err = gErr;
    e->sci = gAnsSci;
    e->exp10 = gAnsExp;
    for (i = 0; i < DEC_BUFLEN; i++)
        e->result[i] = gAnsText[i];
}

/* tokens of groups [0, upto) with each superscript run written as ^( ... ) */
static short BuildTokens(signed char *out, short upto, short max)
{
    short i, n = 0, inexp = 0;
    for (i = 0; i < upto && n < max - 4; i++) {
        if (gGrp[i].raised && !inexp) {
            out[n++] = T_POW;
            out[n++] = T_LPAR;
            inexp = 1;
        } else if (!gGrp[i].raised && inexp) {
            out[n++] = T_RPAR;
            inexp = 0;
        }
        out[n++] = gTok[i];
    }
    if (inexp)
        out[n++] = T_RPAR;
    return n;
}

static const char *ErrText(short e)
{
    switch (e) {
    case DEC_DIV0: return "Can't divide by zero.";
    case DEC_OVERFLOW: return "That number is too big.";
    case DEC_DOMAIN: return "Can't do that power.";
    default: return "That doesn't look like a sum.";
    }
}

static void Decode(void)
{
    short i, depth = 0, n = 0;
    unsigned char tk[MAXTOK * 3 + 12];
    rec_decode(gTok);
    gNTok = gNGrp;
    gEqAt = -1;
    for (i = 0; i < gNTok; i++)
        if (gTok[i] == T_EQ) {
            gEqAt = i;
            break;
        }
    gErr = -1;
    gAnsText[0] = 0;
    if (gEqAt > 0) {
        int ep, e;
        signed char st[MAXTOK * 3];
        short m = BuildTokens(st, gEqAt, MAXTOK * 3);
        for (i = 0; i < m; i++) {
            tk[n++] = (unsigned char)st[i];
            if (st[i] == T_LPAR) depth++;
            if (st[i] == T_RPAR) depth--;
        }
        while (depth-- > 0)                      /* '=' closes open parentheses */
            tk[n++] = T_RPAR;
        e = dec_eval(tk, n, &gAns, &ep);
        gErr = (short)e;
        if (e == DEC_OK) {
            int sci, ex;
            dec_format(&gAns, 12, gAnsText, &sci, &ex);
            gAnsSci = (short)sci;
            gAnsExp = (short)ex;
            dbg.answers++;
        }
        TapeSetCur();
        if (e == DEC_OK)
            SetStatus("Edit > Copy copies the answer.");
        else
            SetStatus(ErrText(gErr));
    } else {
        TapeRemoveCur();
        if (gNTok && gStatusMsg[0] == 'E')
            SetStatus("");
    }
    /* debug block */
    dbg.nTok = gNTok;
    for (i = 0; i < 48; i++) {
        dbg.tok[i] = (signed char)(i < gNTok ? gTok[i] + 32 * gGrp[i].raised : -1);
        dbg.cx[i] = (short)(i < gNGrp ? gPaperR.left + (gGrp[i].x0 + gGrp[i].x1) / 2 : -1);
    }
    dbg.err = gErr;
    for (i = 0; i < 31 && gAnsText[i]; i++)
        dbg.result[i] = gAnsText[i];
    dbg.result[i] = 0;
    if (gErr > 0) {
        const char *t = ErrText(gErr);
        for (i = 0; i < 31 && t[i]; i++)
            dbg.result[i] = t[i];
        dbg.result[i] = 0;
    }
    dbg.tapeN = gTapeN;
    /* redraw the paper only when the handwritten answer (or the ink) changed */
    {
        Drawn d;
        Boolean same;
        d.ok = (short)(gErr == DEC_OK && gEqAt >= 0);
        d.eq = gEqAt;
        d.x1 = (short)(gEqAt >= 0 ? gGrp[gEqAt].x1 : 0);
        d.href = gHref;
        d.sci = gAnsSci;
        d.exp = gAnsExp;
        for (i = 0; i < DEC_BUFLEN; i++)
            d.text[i] = d.ok ? gAnsText[i] : 0;
        same = d.ok == gLastDrawn.ok && d.eq == gLastDrawn.eq && d.x1 == gLastDrawn.x1 &&
               d.href == gLastDrawn.href && d.sci == gLastDrawn.sci && d.exp == gLastDrawn.exp;
        for (i = 0; same && i < DEC_BUFLEN; i++)
            same = d.text[i] == gLastDrawn.text[i];
        if (gPaperDirty || !same) {
            gLastDrawn = d;
            gPaperDirty = false;
            PaperRender();
            PaperShow(NULL);
        }
    }
    DrawStrip();
    if (gTapeDirty) {
        gTapeDirty = false;
        DrawTape();
    }
}

void LineIdle(void)
{
    short k = (short)rec_next_pending();
    dbg.pending = 0;
    if (k >= 0) {
        unsigned long t0;
        short j, p = 0;
        for (j = 0; j < gNGrp; j++)
            p = (short)(p + !gGrp[j].ready);
        dbg.pending = p;
        /* let the writer finish a symbol first: wait a moment after the last stroke, except for
           symbols that are clearly finished (the newest stroke belongs to another symbol) */
        if (TickCount() - gLastInkTick < 10 || Button())
            return;
        if (TickCount() - gLastInkTick < 20) {
            short newest = (short)(gNStk - 1), j2;
            for (j2 = 0; j2 < gGrp[k].n; j2++)
                if (gGrp[k].stk[j2] == newest)
                    return;
        }
        t0 = TickCount();
        if (!rec_classify(k)) {
            dbg.recogAborts++;
            return;
        }
        dbg.recogTicks += TickCount() - t0;
        dbg.recogCount++;
        gNeedDecode = true;
        if (rec_next_pending() >= 0)
            DrawStrip();
        return;
    }
    if (gNeedDecode && !Button()) {
        gNeedDecode = false;
        Decode();
    }
}

int ClassifyRaster(const unsigned char *bits, short *scores)
{
    return net_forward(bits, scores, NULL);
}

static int MouseDown(void)
{
    return Button();
}

void LineInit(void)
{
    gNStk = gNPts = 0;
    gTapeN = 0;
    gTapeCur = -1;
    ForgetUndo();
    gRecAbort = MouseDown;
    rec_segment();
    gLastDrawn.ok = -1;
}

/*
 * Scribble Calc: grouping strokes into symbols, the symbol raster, the classifier cache and the
 * grammar decoder. Integers only, no Toolbox calls: the same file builds on the host for the
 * bit-exact tests against calc/ref/decode.py (the specification; read its docstring first).
 */
#include "ink.h"
#include "net_gen.h"

Pt gPts[MAXPTS];
Stroke gStk[MAXSTK];
short gNPts, gNStk;
Group gGrp[MAXGRP];
short gNGrp, gHref;

#define TOL 2

static short sW[MAXSTK], sH[MAXSTK], sCx2[MAXSTK];
static unsigned char sTiny[MAXSTK], sFlat[MAXSTK];
static Group sWork[MAXGRP];

static short iabs(short v) { return v < 0 ? (short)-v : v; }
static short imax(short a, short b) { return a > b ? a : b; }
static short imin(short a, short b) { return a < b ? a : b; }

static short ref_height(void)
{
    static short hs[MAXSTK];
    short n = 0, i, j;
    for (i = 0; i < gNStk; i++) {
        short h;
        if (!gStk[i].alive)
            continue;
        h = (short)(gStk[i].y1 - gStk[i].y0);
        if (h > 8) {
            for (j = n; j > 0 && hs[j - 1] > h; j--)
                hs[j] = hs[j - 1];
            hs[j] = h;
            n++;
        }
    }
    if (!n)
        return 32;
    return imax(16, hs[n / 2]);
}

static void add_stroke(Group *g, short i)
{
    Stroke *s = &gStk[i];
    g->stk[g->n++] = i;
    g->x0 = imin(g->x0, s->x0);
    g->x1 = imax(g->x1, s->x1);
    g->y0 = imin(g->y0, s->y0);
    g->y1 = imax(g->y1, s->y1);
    g->tiny = (unsigned char)(g->tiny && sTiny[i]);
    if (sFlat[i]) {
        if (!g->hasbar) {
            g->bar0 = s->x0;
            g->bar1 = s->x1;
            g->bary0 = s->y0;
            g->bary1 = s->y1;
            g->hasbar = 1;
        } else {
            g->bar0 = imin(g->bar0, s->x0);
            g->bar1 = imax(g->bar1, s->x1);
            g->bary0 = imin(g->bary0, s->y0);
            g->bary1 = imax(g->bary1, s->y1);
        }
    }
    if (!sTiny[i] && !sFlat[i])
        g->hastall = 1;
}

static short orient(const Pt *a, const Pt *b, const Pt *c)
{
    long v = (long)(b->x - a->x) * (c->y - a->y) - (long)(b->y - a->y) * (c->x - a->x);
    return (short)((v > 0) - (v < 0));
}

static short seg_touch(const Pt *a, const Pt *b, const Pt *c, const Pt *d)
{
    if (imax(a->x, b->x) < imin(c->x, d->x) || imax(c->x, d->x) < imin(a->x, b->x) ||
        imax(a->y, b->y) < imin(c->y, d->y) || imax(c->y, d->y) < imin(a->y, b->y))
        return 0;
    return orient(a, b, c) * orient(a, b, d) <= 0 && orient(c, d, a) * orient(c, d, b) <= 0;
}

/* does stroke i cross or touch stroke k (decode.py crosses); segments are skipped quickly when
   their boxes can't meet */
static short crosses(short i, short k)
{
    const Stroke *s = &gStk[i], *t = &gStk[k];
    const Pt *p = gPts + s->first, *q = gPts + t->first;
    short ns = (short)(s->n > 1 ? s->n - 1 : 1), nt = (short)(t->n > 1 ? t->n - 1 : 1), a, b;
    if (s->x1 < t->x0 || t->x1 < s->x0 || s->y1 < t->y0 || t->y1 < s->y0)
        return 0;
    for (a = 0; a < ns; a++) {
        const Pt *a0 = p + a, *a1 = s->n > 1 ? p + a + 1 : p + a;
        short ax0 = imin(a0->x, a1->x), ax1 = imax(a0->x, a1->x), ay0 = imin(a0->y, a1->y), ay1 = imax(a0->y, a1->y);
        if (ax1 < t->x0 || t->x1 < ax0 || ay1 < t->y0 || t->y1 < ay0)
            continue;
        for (b = 0; b < nt; b++) {
            const Pt *b0 = q + b, *b1 = t->n > 1 ? q + b + 1 : q + b;
            if (ax1 < imin(b0->x, b1->x) || imax(b0->x, b1->x) < ax0 ||
                ay1 < imin(b0->y, b1->y) || imax(b0->y, b1->y) < ay0)
                continue;
            if (seg_touch(a0, a1, b0, b1))
                return 1;
        }
    }
    return 0;
}

/* a point of stroke i within r of a point of stroke k, or they cross (decode.py near) */
static short near(short i, short k, short r)
{
    const Stroke *s = &gStk[i], *t = &gStk[k];
    const Pt *p = gPts + s->first, *q = gPts + t->first;
    long r2 = (long)r * r;
    short a, b;
    if (s->x1 + r < t->x0 || t->x1 + r < s->x0 || s->y1 + r < t->y0 || t->y1 + r < s->y0)
        return 0;
    for (a = 0; a < s->n; a++) {
        short px = p[a].x, py = p[a].y;
        if (px + r < t->x0 || px - r > t->x1 || py + r < t->y0 || py - r > t->y1)
            continue;
        for (b = 0; b < t->n; b++) {
            long dx = px - q[b].x, dy = py - q[b].y;
            if (dx * dx + dy * dy <= r2)
                return 1;
        }
    }
    return crosses(i, k);
}

static short group_near(const Group *g, short i, short r, short crossonly)
{
    short j;
    for (j = 0; j < g->n; j++)
        if (crossonly ? crosses(i, g->stk[j]) : near(i, g->stk[j], r))
            return 1;
    return 0;
}

/* which groups may be superscripts (decode.py raised_flags) */
static void rec_allow(void)
{
    short i, anchor = -1, tiny_t = imax(3, gHref / 4);
    for (i = 0; i < gNGrp; i++) {
        Group *g = &gGrp[i];
        short w = (short)(g->x1 - g->x0), h = (short)(g->y1 - g->y0);
        short tiny = imax(w, h) <= tiny_t, flat = !tiny && w >= 2 * h;
        g->allowR = 0;
        if (anchor >= 0 && i > 0) {
            const Group *a = &gGrp[anchor];
            short ah = (short)(a->y1 - a->y0);
            short looks = !tiny && !flat && 5L * h <= 4L * ah && 5L * (g->y1 - a->y0) <= 3L * ah &&
                          5L * (g->y0 - a->y0) <= ah;
            if (anchor == i - 1)
                g->allowR = (unsigned char)(looks && 2L * (g->x0 - a->x1) <= ah);
            else if (gGrp[i - 1].allowR)
                g->allowR = (unsigned char)(2L * (g->x0 - gGrp[i - 1].x1) <= ah &&
                    (looks || ((tiny || flat) && 3L * ((g->y0 + g->y1) / 2 - a->y0) <= ah)));
        }
        if (!g->allowR && !tiny && !flat && 5L * h >= 3L * gHref)
            anchor = i;
    }
}

int rec_segment(void)
{
    short i, k, nw = 0, href, tiny_t, near_t, near_r;
    href = ref_height();
    gHref = href;
    tiny_t = imax(3, href / 4);
    near_r = imax(3, href / 10);
    near_t = (short)(2 * imax(href / 6, 3));
    for (i = 0; i < gNStk; i++) {
        Stroke *s = &gStk[i];
        if (!s->alive)
            continue;
        sW[i] = (short)(s->x1 - s->x0);
        sH[i] = (short)(s->y1 - s->y0);
        sCx2[i] = (short)(s->x0 + s->x1);
        sTiny[i] = (unsigned char)(imax(sW[i], sH[i]) <= tiny_t);
        sFlat[i] = (unsigned char)(!sTiny[i] && sW[i] >= 2 * sH[i]);
    }
    for (i = 0; i < gNStk; i++) {
        Stroke *s = &gStk[i];
        Group *g;
        short best = -1, bestov = 0, cx, cy;
        if (!s->alive)
            continue;
        cx = (short)(sCx2[i] / 2);
        cy = (short)((s->y0 + s->y1) / 2);
        for (k = 0; k < nw; k++) {
            short ok = 0, ov, gw, gh;
            g = &sWork[k];
            if (g->n == 0 || g->n >= GRPSTK)
                continue;
            gw = (short)(g->x1 - g->x0);
            gh = (short)(g->y1 - g->y0);
            ov = (short)(imin(s->x1, g->x1) - imax(s->x0, g->x0));
            if (sTiny[i]) {
                if (g->hasbar && !g->hastall)
                    ok = g->bar0 - TOL <= cx && cx <= g->bar1 + TOL &&
                         2 * iabs((short)(cy - (g->bary0 + g->bary1) / 2)) <= href;
                else if (g->tiny)
                    ok = iabs((short)(sCx2[i] - (g->x0 + g->x1))) <= near_t;
                else if (g->hastall)
                    ok = g->x0 <= cx && cx <= g->x1 && g->y0 <= cy && 4L * (cy - g->y0) <= 3L * gh;
            } else if (sFlat[i]) {
                if (g->tiny) {
                    short gc = (short)((g->x0 + g->x1) / 2), gcy = (short)((g->y0 + g->y1) / 2);
                    ok = s->x0 - TOL <= gc && gc <= s->x1 + TOL && 2 * iabs((short)(gcy - cy)) <= href;
                } else {
                    ok = g->x0 - TOL <= cx && cx <= g->x1 + TOL;
                    if (!ok && g->hastall && 4L * (cy - g->y0) <= gh)
                        ok = group_near(g, i, near_r, 0);
                }
            } else {
                if (g->tiny) {
                    short gx = (short)((g->x0 + g->x1) / 2), gy = (short)((g->y0 + g->y1) / 2);
                    ok = s->x0 <= gx && gx <= s->x1 && s->y0 <= gy && 4L * (gy - s->y0) <= 3L * sH[i];
                } else if (!g->hastall) {
                    ok = 20L * (g->x0 - cx) <= -3L * gw && 20L * (cx - g->x1) <= -3L * gw;
                    if (!ok && g->hasbar && 4L * ((g->bary0 + g->bary1) / 2 - s->y0) <= sH[i])
                        ok = group_near(g, i, near_r, 0);
                } else {
                    ok = 10L * ov >= 7L * imin(sW[i], gw) ||
                         20L * iabs((short)(sCx2[i] - (g->x0 + g->x1))) <= 2L * (7L * imax(sW[i], gw) + 20 * TOL);
                    if (!ok && 4L * ov >= imin(sW[i], gw))
                        ok = group_near(g, i, 0, 1);
                }
            }
            if (ok && (best < 0 || ov >= bestov)) {     /* most overlap; ties: the latest group */
                best = k;
                bestov = ov;
            }
        }
        if (best >= 0) {
            g = &sWork[best];
            add_stroke(g, i);
        } else if (nw < MAXGRP) {
            best = nw;
            g = &sWork[nw++];
            g->n = 0;
            g->x0 = s->x0; g->x1 = s->x1; g->y0 = s->y0; g->y1 = s->y1;
            g->hasbar = 0;
            g->tiny = 1;
            g->hastall = 0;
            add_stroke(g, i);
        } else {
            continue;
        }
        if (sFlat[i] && !g->hastall) {               /* a bar takes the dots of a divide sign */
            for (k = 0; k < nw; k++) {
                Group *g2 = &sWork[k];
                short gc, gcy, j;
                if (k == best || g2->n == 0 || !g2->tiny)
                    continue;
                gc = (short)((g2->x0 + g2->x1) / 2);
                gcy = (short)((g2->y0 + g2->y1) / 2);
                if (s->x0 - TOL <= gc && gc <= s->x1 + TOL && 2 * iabs((short)(gcy - cy)) <= href &&
                    g->n + g2->n <= GRPSTK) {
                    for (j = 0; j < g2->n; j++)
                        add_stroke(g, g2->stk[j]);
                    g2->n = 0;
                }
            }
        }
    }
    /* compact, sort strokes in each group, sort groups by (x0 + x1, first stroke) */
    gNGrp = 0;
    for (k = 0; k < nw; k++) {
        Group *g = &sWork[k];
        short a, b, j;
        if (g->n == 0)
            continue;
        for (a = 1; a < g->n; a++) {
            short v = g->stk[a];
            for (b = a; b > 0 && g->stk[b - 1] > v; b--)
                g->stk[b] = g->stk[b - 1];
            g->stk[b] = v;
        }
        g->key = g->stk[0];
        for (j = gNGrp; j > 0; j--) {
            Group *p = &gGrp[j - 1];
            long kp = (long)(p->x0 + p->x1), kg = (long)(g->x0 + g->x1);
            if (kp < kg || (kp == kg && p->key < g->key))
                break;
            gGrp[j] = *p;
        }
        gGrp[j] = *g;
        gNGrp++;
    }
    for (k = 0; k < gNGrp; k++) {
        Group *g = &gGrp[k];
        Stroke *ks = &gStk[g->key];
        unsigned long h = 2166136261UL;
        short j;
        for (j = 0; j < g->n; j++)
            h = (h ^ (unsigned long)g->stk[j]) * 16777619UL;
        h = (h ^ (unsigned long)(href / 2)) * 16777619UL;
        g->hash = h;
        g->ready = (unsigned char)(ks->cached && ks->ckey == h);
        g->sc = ks->sc;
        g->forced = (ks->force != NOFORCE && ks->forceN == g->n) ? ks->force : NOFORCE;
        g->cls = -1;
        g->raised = 0;
    }
    rec_allow();
    return gNGrp;
}

/* ------------------------------------------------------------------ raster (strokes.py) */

#define RG 24
#define RB 20

static unsigned char sImg[25 * 25];

static void stamp(short x, short y)
{
    unsigned char *p = sImg + y * 25 + x;
    p[0] = p[1] = p[25] = p[26] = 1;
}

static void bres(short x0, short y0, short x1, short y1)
{
    short dx = iabs((short)(x1 - x0)), sx = x0 < x1 ? 1 : -1;
    short dy = (short)-iabs((short)(y1 - y0)), sy = y0 < y1 ? 1 : -1;
    short err = (short)(dx + dy), e2;
    for (;;) {
        stamp(x0, y0);
        if (x0 == x1 && y0 == y1)
            return;
        e2 = (short)(2 * err);
        if (e2 >= dy) {
            err = (short)(err + dy);
            x0 = (short)(x0 + sx);
        }
        if (e2 <= dx) {
            err = (short)(err + dx);
            y0 = (short)(y0 + sy);
        }
    }
}

void rec_raster(const Group *g, short minD, unsigned char *bits)
{
    short x0 = g->x0, y0 = g->y0, w = (short)(g->x1 - g->x0), h = (short)(g->y1 - g->y0);
    short D = imax(imax(w, h), imax(minD, 1)), D2 = (short)(D / 2);
    short sw = (short)(((long)w * RB + D2) / D), sh = (short)(((long)h * RB + D2) / D);
    short ox = (short)((RG - 1 - sw) / 2), oy = (short)((RG - 1 - sh) / 2);
    short j, i, y;
    for (i = 0; i < 25 * 25; i++)
        sImg[i] = 0;
    for (j = 0; j < g->n; j++) {
        const Stroke *s = &gStk[g->stk[j]];
        const Pt *p = gPts + s->first;
        short px = (short)(ox + ((long)(p[0].x - x0) * RB + D2) / D);
        short py = (short)(oy + ((long)(p[0].y - y0) * RB + D2) / D);
        if (s->n == 1) {
            stamp(px, py);
            continue;
        }
        for (i = 1; i < s->n; i++) {
            short qx = (short)(ox + ((long)(p[i].x - x0) * RB + D2) / D);
            short qy = (short)(oy + ((long)(p[i].y - y0) * RB + D2) / D);
            bres(px, py, qx, qy);
            px = qx;
            py = qy;
        }
    }
    for (y = 0; y < RG; y++) {
        const unsigned char *r = sImg + y * 25;
        for (j = 0; j < 3; j++) {
            unsigned char v = 0;
            for (i = 0; i < 8; i++)
                v = (unsigned char)((v << 1) | r[j * 8 + i]);
            bits[y * 3 + j] = v;
        }
    }
}

int rec_next_pending(void)
{
    short k;
    for (k = 0; k < gNGrp; k++)
        if (!gGrp[k].ready)
            return k;
    return -1;
}

int (*gRecAbort)(void);

int rec_classify(int gi)
{
    Group *g = &gGrp[gi];
    Stroke *ks = &gStk[g->key];
    unsigned char bits[72];
    rec_raster(g, (short)(gHref / 2), bits);
    if (net_forward(bits, ks->sc, gRecAbort))
        return 0;                                /* interrupted (the mouse went down) */
    ks->cached = 1;
    ks->ckey = g->hash;
    g->ready = 1;
    g->sc = ks->sc;
    return 1;
}

/* ------------------------------------------------------------------ grammar decoding (decode.py) */

#define MAXD 8
#define NST 8
#define NS (NST * (MAXD + 1))
#define NEG (-1000000000L)
#define PEN_AUTOCLOSE 24
#define PEN_TAIL 32
enum { START, UNARY, INT, DOT, FRAC, CLOSE, END, TAIL };
enum { C_PLUS = 10, C_MINUS, C_TIMES, C_DIV, C_EQ, C_LPAR, C_RPAR, C_POINT };

/* returns the next state index (state * (MAXD + 1) + depth) or -1; *pen = penalty */
static short step(short state, short depth, short c, short *pen)
{
    *pen = 0;
    if (state == END || state == TAIL) {
        *pen = PEN_TAIL;
        return TAIL * (MAXD + 1);
    }
    if (c <= 9) {
        if (state == START || state == UNARY || state == INT)
            return (short)(INT * (MAXD + 1) + depth);
        if (state == DOT || state == FRAC)
            return (short)(FRAC * (MAXD + 1) + depth);
        return -1;
    }
    switch (c) {
    case C_POINT:
        if (state == START || state == UNARY)
            return (short)(DOT * (MAXD + 1) + depth);
        if (state == INT)
            return (short)(FRAC * (MAXD + 1) + depth);
        return -1;
    case C_PLUS:
    case C_TIMES:
    case C_DIV:
        return (state == INT || state == FRAC || state == CLOSE) ? (short)(START * (MAXD + 1) + depth) : -1;
    case C_MINUS:
        if (state == INT || state == FRAC || state == CLOSE)
            return (short)(START * (MAXD + 1) + depth);
        if (state == START)
            return (short)(UNARY * (MAXD + 1) + depth);
        return -1;
    case C_LPAR:
        if (depth >= MAXD)
            return -1;
        if (state == START || state == UNARY || state == INT || state == FRAC || state == CLOSE)
            return (short)(START * (MAXD + 1) + depth + 1);
        return -1;
    case C_RPAR:
        return ((state == INT || state == FRAC || state == CLOSE) && depth > 0) ?
               (short)(CLOSE * (MAXD + 1) + depth - 1) : -1;
    case C_EQ:
        if (state == INT || state == FRAC || state == CLOSE) {
            *pen = (short)(PEN_AUTOCLOSE * depth);
            return END * (MAXD + 1);
        }
        return -1;
    }
    return -1;
}

#define NSE (2 * NS)                          /* states x "inside an exponent" */
#define RAISE_BONUS 16

static unsigned char sBpSt[MAXGRP][NSE], sBpC[MAXGRP][NSE];

int rec_decode(signed char *tokens)
{
    static long best[NSE], nb[NSE];
    short i, st, c, k, n = gNGrp;
    long total;
    for (i = 0; i < n; i++)
        if (!gGrp[i].ready)
            return 0;
    if (n == 0)
        return 1;
    for (k = 0; k < NSE; k++)
        best[k] = NEG;
    best[START * (MAXD + 1)] = 0;
    for (i = 0; i < n; i++) {
        const short *sc = gGrp[i].sc;
        short c0 = 0, c1 = NCLS - 1, e1 = gGrp[i].allowR ? 1 : 0;
        if (gGrp[i].forced != NOFORCE)
            c0 = c1 = gGrp[i].forced;
        for (k = 0; k < NSE; k++)
            nb[k] = NEG;
        for (st = 0; st < NSE; st++) {
            short eprev, state0, depth, e;
            if (best[st] == NEG)
                continue;
            eprev = (short)(st / NS);
            state0 = (short)((st % NS) / (MAXD + 1));
            depth = (short)(st % (MAXD + 1));
            for (e = 0; e <= e1; e++) {
                short state = state0;
                if (e != eprev && state != END && state != TAIL) {
                    if (state != INT && state != FRAC && state != CLOSE)
                        continue;
                    state = e ? START : CLOSE;
                }
                for (c = c0; c <= c1; c++) {
                    short pen, ns, ee;
                    long v;
                    if (e && (c == C_LPAR || c == C_RPAR || c == C_EQ))
                        continue;
                    ns = step(state, depth, c, &pen);
                    if (ns < 0)
                        continue;
                    ee = (ns / (MAXD + 1) == END || ns / (MAXD + 1) == TAIL) ? 0 : e;
                    ns = (short)(ns + ee * NS);
                    v = best[st] + sc[c] - pen + (e ? RAISE_BONUS : 0);
                    if (v > nb[ns]) {
                        nb[ns] = v;
                        sBpSt[i][ns] = (unsigned char)st;
                        sBpC[i][ns] = (unsigned char)(c + 18 * e);
                    }
                }
            }
        }
        for (k = 0; k < NSE; k++)
            best[k] = nb[k];
    }
    k = 0;
    for (st = 1; st < NSE; st++)
        if (best[st] > best[k])
            k = st;
    total = best[k];
    if (total <= NEG / 2) {
        for (i = 0; i < n; i++) {
            short bc = 0;
            if (gGrp[i].forced != NOFORCE)
                bc = gGrp[i].forced;
            else
                for (c = 1; c < NCLS; c++)
                    if (gGrp[i].sc[c] > gGrp[i].sc[bc])
                        bc = c;
            tokens[i] = gGrp[i].cls = (signed char)bc;
            gGrp[i].raised = 0;
        }
        return 1;
    }
    for (i = (short)(n - 1); i >= 0; i--) {
        c = sBpC[i][k];
        tokens[i] = gGrp[i].cls = (signed char)(c % 18);
        gGrp[i].raised = (unsigned char)(c >= 18);
        k = sBpSt[i][k];
    }
    return 1;
}

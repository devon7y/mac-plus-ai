/* cast_model.c: a C transliteration of app/cast.s (same RCtx fields, same run-area layout, same
 * control flow), for debugging the 68000 caster's logic on the host. Built into host_render
 * with -DCAST_MODEL, where it replaces cast_columns(). Keep it in step with cast.s. */
#include <string.h>
#include "render.h"
#include "world.h"

#define RUNBYTES 1440
#define NGMAX 39
#define DSPLIT 513
#define NONE (-32768)
#define KTOP 0x1000
#define KBOT 0x2000

typedef struct Rec { short key; const Seg *s; short df; } Rec;
typedef struct SB { const Seg *s; short y0, y1; } SB;
static Rec recs[R_MAXREC];
static SB sbuf[R_MAXSB];

typedef struct M {
    RCtx *x;
    char *a5;                 /* column runs (bytes) */
    const short *a1, *a3;     /* far / near height tables */
    short d5, d6, d7, d4;     /* H0, rn, rf, chain */
} M;

#define RUN(off) ((RRun *)(m->a5 + (off)))
#define SEGP(p) ((const Seg *)(p))

static short PROJ(const short *tab, short h2, short r, short h0)
{
    long p = (long)tab[h2 / 2] * r;
    return (short)(h0 - (short)(p >> 16));
}

static void linkfast(M *m, short *d1out)
{
    RCtx *x = m->x;
    short d2 = (short)((char *)x->topsp - m->a5), d0, d3, d1 = 0;
    for (d0 = 12; d0 <= d2; d0 += 12) RUN(d0 - 12)->next = d0;
    d3 = (short)((char *)x->botsp - m->a5);
    if (x->ng) {
        RRun *g = (RRun *)x->topsp;
        g->yend = x->gend; g->id = -1; g->key = 0; g->d = 0x7FFF; g->next = d3;
    } else if (d2 == 0)
        d1 = d3;
    else
        RUN(d2 - 12)->next = d3;
    for (d0 = d3 + 12; d0 <= RUNBYTES; d0 += 12) RUN(d0 - 12)->next = d0;
    *d1out = d1;
}

static void pend(M *m)
{
    RCtx *x = m->x;
    short *last = x->gaps0 + 3 * x->ng;
    x->gstart = x->gaps[0];
    x->lastg = last;
    x->gend = last[1];
}

static void pgen(M *m, short d0, short d1)
{
    RCtx *x = m->x;
    short *g = x->gaps;
    for (;;) {
        short d2, d3;
        if (d0 >= g[1]) { g += 3; continue; }
        if (d1 <= g[0]) break;
        if (x->nnlim < x->nn || x->ng >= NGMAX) break;
        d2 = g[2];
        if (d0 <= g[0]) {
            if (d1 >= g[1]) {                       /* whole gap */
                RUN(d2)->id = x->pid; RUN(d2)->key = x->pkey; RUN(d2)->d = x->depth;
                x->ng--;
                memmove(g, g + 3, ((x->ng - (int)((g - x->gaps) / 3)) + 1) * 6);
                continue;
            }
            d3 = x->nn;                             /* top */
            RUN(d3)->yend = g[1]; RUN(d3)->id = -1; RUN(d3)->key = 0; RUN(d3)->d = 0x7FFF;
            RUN(d3)->next = RUN(d2)->next;
            RUN(d2)->yend = d1; RUN(d2)->id = x->pid; RUN(d2)->key = x->pkey; RUN(d2)->d = x->depth;
            RUN(d2)->next = d3;
            g[0] = d1; g[2] = d3;
            x->nn += 12;
            break;
        }
        if (d1 >= g[1]) {                           /* bottom */
            d3 = x->nn;
            RUN(d3)->yend = g[1]; RUN(d3)->id = x->pid; RUN(d3)->key = x->pkey; RUN(d3)->d = x->depth;
            RUN(d3)->next = RUN(d2)->next;
            RUN(d2)->next = d3;
            RUN(d2)->yend = d0;
            g[1] = d0;
            x->nn += 12;
            g += 3;
            continue;
        }
        d3 = x->nn;                                 /* middle */
        RUN(d3)->yend = d1; RUN(d3)->id = x->pid; RUN(d3)->key = x->pkey; RUN(d3)->d = x->depth;
        RUN(d3 + 12)->yend = g[1]; RUN(d3 + 12)->id = -1; RUN(d3 + 12)->key = 0; RUN(d3 + 12)->d = 0x7FFF;
        RUN(d3 + 12)->next = RUN(d2)->next;
        RUN(d2)->next = d3;
        RUN(d2)->yend = d0;
        RUN(d3)->next = d3 + 12;
        {
            short *e = g + 3;
            while (*e != 32767) e += 3;               /* end sentinel */
            memmove(g + 6, g + 3, (size_t)((char *)e - (char *)(g + 3)) + 6);
        }
        g[3] = d1; g[4] = g[1]; g[5] = d3 + 12;
        g[1] = d0;
        x->ng++;
        x->nn += 24;
        break;
    }
    pend(m);
}

static void paint(M *m, short d0, short d1)
{
    RCtx *x = m->x;
    x->npaint++;
    if (d0 >= d1) return;
    if (!x->slow) {
        short gs = x->gstart, ge = x->gend;
        if (d1 <= gs || d0 >= ge) return;
        if (d1 >= ge) {
            RRun *r = x->botsp - 1;
            if ((char *)r - (char *)x->topsp < 12) return;
            x->botsp = r;
            r->yend = x->gend; r->id = x->pid; r->key = x->pkey; r->d = x->depth;
            x->nfast++;
            if (d0 <= gs) { x->ng = 0; x->gstart = 32767; x->gend = -32768; }
            else x->gend = d0;
            return;
        }
        if (d0 <= gs) {
            RRun *r = x->topsp;
            if ((char *)x->botsp - (char *)r < 24) return;
            r->yend = d1; r->id = x->pid; r->key = x->pkey; r->d = x->depth;
            x->topsp = r + 1;
            x->gstart = d1;
            x->nfast++;
            return;
        }
        {                                           /* convert */
            short dd, gap;
            linkfast(m, &dd);
            gap = (short)((char *)x->topsp - m->a5);
            x->lastg = x->gaps;
            x->gaps[0] = x->gstart; x->gaps[1] = x->gend; x->gaps[2] = gap;
            x->gaps[3] = 32767; x->gaps[4] = 32767;
            x->nn = gap + 12;
            x->nnlim = (short)((char *)x->botsp - m->a5) - 24;
            x->slow = 1;
            pgen(m, d0, d1);
            return;
        }
    }
    if (d0 < 0) d0 = 0;
    if (d1 > x->H) d1 = x->H;
    if (d0 >= d1) return;
    {
        short d2 = x->gend;
        if (d1 >= d2) {
            short *g;
            if (d0 >= d2) return;
            g = x->lastg;
            if (d0 > g[0]) {
                short n, d3;
                if (x->nnlim < x->nn || x->ng >= NGMAX) return;
                n = g[2]; d3 = x->nn;
                RUN(d3)->yend = x->gend; RUN(d3)->id = x->pid; RUN(d3)->key = x->pkey; RUN(d3)->d = x->depth;
                RUN(d3)->next = RUN(n)->next;
                RUN(n)->next = d3;
                RUN(n)->yend = d0;
                g[1] = d0;
                x->gend = d0;
                x->nn += 12;
                return;
            }
        }
    }
    pgen(m, d0, d1);
}

static short near_y(M *m, short h2)
{
    RCtx *x = m->x;
    if (x->ncache) {
        if (x->pch0 == h2) return x->pcy0;
        if (x->ncache == 2 && x->pch1 == h2) return x->pcy1;
    }
    return PROJ(m->a3, h2, m->d6, m->d5);
}

static void cachestore(M *m, short h2, short y)
{
    RCtx *x = m->x;
    if (x->nch < 2) {
        if (x->nch == 0) { x->ch0 = h2; x->cy0 = y; } else { x->ch1 = h2; x->cy1 = y; }
        x->nch++;
    }
}

static void sidekeys(M *m)
{
    RCtx *x = m->x;
    short k = x->rkey;
    x->sface = (k & 0x4000) ? 8 : 4;
    x->pkey = (x->vf & 1) ? 0 : k;
    x->depth = x->dn;
}

static void sidepieces(M *m, const Seg *s)
{
    RCtx *x = m->x;
    short d1 = x->sy0;
    x->sline = 0;
    if (m->a3[s->top] > 0) { d1++; x->sline = 1; }
    x->sy0p = d1;
    x->syb = d1;
    if (s->type == TURF) {
        const short *b = x->dn < DSPLIT ? x->b16 : x->b8;
        short yb = PROJ(b, (short)(s->top * 2), m->d6, m->d5);
        if (yb < x->sy0p) yb = x->sy0p;
        x->syb = yb;
    }
}

static short sideid(M *m, const Seg *s) { return (short)(((s->type << 5) | m->x->vf) + m->x->sface); }
static short lineid(M *m) { return (short)((m->x->vf & 1) + 4); }

void cast_columns(RCtx *x)
{
    M mm, *m = &mm;
    short col;
    m->x = x;
    for (col = 0; col < x->ncols; col++) {
        short rdx, rdz, adx, adz, ddx, ddz, sdx, sdz, kx, kz, sx, sz;
        long xstep, zstep;
        const char *cp;
        Rec *rec, *recend;
        m->a5 = (char *)x->runs + (long)col * RUNBYTES;
        x->col = col;
        {
            short cx = x->camx[col];
            rdx = (short)(x->dirx + (short)(((long)x->plx * cx * 4) >> 16));
            rdz = (short)(x->dirz + (short)(((long)x->plz * cx * 4) >> 16));
        }
        adx = rdx < 0 ? -rdx : rdx;
        ddx = 0x7FFF;
        if (adx >= 128) { unsigned long q = 0x400000UL / (unsigned short)adx; if (q <= 0x7FFF) ddx = (short)q; }
        kx = x->kx0;
        if (rdx > 0) { sdx = (short)(((unsigned long)(256 - x->fx) * (unsigned short)ddx) >> 8); xstep = x->pwx4; sx = 1; }
        else { sdx = (short)(((unsigned long)x->fx * (unsigned short)ddx) >> 8); xstep = -x->pwx4; kx++; sx = -1; }
        adz = rdz < 0 ? -rdz : rdz;
        ddz = 0x7FFF;
        if (adz >= 128) { unsigned long q = 0x400000UL / (unsigned short)adz; if (q <= 0x7FFF) ddz = (short)q; }
        kz = x->kz0;
        if (rdz > 0) { sdz = (short)(((unsigned long)(256 - x->fz) * (unsigned short)ddz) >> 8); zstep = 4; sz = 1; }
        else { sdz = (short)(((unsigned long)x->fz * (unsigned short)ddz) >> 8); zstep = -4; kz++; sz = -1; }
        /* A */
        cp = (const char *)x->cp0;
        rec = recs;
        rec->key = 0;
        for (;;) {
            unsigned short ux = (unsigned short)sdx, uz = (unsigned short)sdz, df;
            if (ux < uz) {
                df = ux;
                if (df >= (unsigned short)x->dmax) break;
                rec->s = *(Seg *const *)cp; rec->df = (short)df; rec++;
                cp += xstep / 4 * (long)sizeof(Seg *); sdx = (short)(ux + (unsigned short)ddx); kx += sx; rec->key = kx;
            } else {
                df = uz;
                if (df >= (unsigned short)x->dmax) break;
                rec->s = *(Seg *const *)cp; rec->df = (short)df; rec++;
                cp += zstep / 4 * (long)sizeof(Seg *); sdz = (short)(uz + (unsigned short)ddz); kz += sz; rec->key = kz;
            }
        }
        rec->s = *(Seg *const *)cp; rec->df = x->dmax; rec++;
        recend = rec;
        if (x->mode) continue;
        /* B */
        x->topsp = (RRun *)m->a5;
        x->botsp = (RRun *)(m->a5 + RUNBYTES);

        x->slow = 0; x->ng = 1; x->gstart = 0; x->gend = x->H;
        x->dn = 0; x->ncache = 0; x->vf = x->var0;
        m->d6 = x->recip[0]; m->a3 = x->t16; m->d5 = x->H0;
        rec = recs;
        while (rec < recend) {
            const Seg *s, *s0, *k;
            x->ncell++;
            x->rkey = rec->key;
            s = rec->s;
            x->df = rec->df;
            rec++;
            m->d7 = x->recip[(unsigned short)x->df];
            m->a1 = (unsigned short)x->df >= DSPLIT ? x->t8 : x->t16;
            x->nch = 0;
            x->phase = 1;
            s0 = s; k = s;
            if (!s->top) goto bdone;
            if (m->a1[s->top] > 0) {
                while (k->top && m->a1[k->bot] > 0) k++;
                x->seg0 = s0; x->segk = k;
                if (!k->top || m->a1[k->top] <= 0) goto above;
                x->phase = 0;
                s = k;
            }
        brun:
            for (;;) {
                const Seg *runend, *p;
                SB *sb;
                if (!s->top) goto bdone;
                x->topf = 0; m->d4 = NONE; x->runstart = s;
                runend = x->phase ? s + (s->flags >> 2) : s + 1;
                x->runend = runend;
                if ((s->flags & SEG_TOPEXP) && m->a1[s->top] <= 0) {
                    short yf = PROJ(m->a1, (short)(s->top * 2), m->d7, m->d5);
                    cachestore(m, (short)(s->top * 2), yf);
                    if (yf >= x->gend) goto bdone;
                    if (m->a1[s->top] != 0) {
                        short yn;
                        x->ty0 = yf;
                        yn = near_y(m, (short)(s->top * 2));
                        m->d4 = yn;
                        if (yn > x->gstart) { x->ty1 = yn; x->topf = 1; }
                    }
                }
                if (x->rkey) {
                    sb = sbuf;
                    for (p = s; p < runend; p++) {
                        short y0, y1;
                        if (m->d4 == NONE) m->d4 = PROJ(m->a3, (short)(p->top * 2), m->d6, m->d5);
                        if (m->d4 >= x->gend) break;
                        y0 = m->d4;
                        y1 = m->d4 = PROJ(m->a3, (short)(p->bot * 2), m->d6, m->d5);
                        if (y1 > x->gstart) { sb->s = p; sb->y0 = y0; sb->y1 = y1; sb++; }
                    }
                    if (sb != sbuf) {
                        sidekeys(m);
                        while (sb > sbuf) {
                            const Seg *q;
                            sb--;
                            q = sb->s;
                            x->sy0 = sb->y0; x->sy1 = sb->y1;
                            sidepieces(m, q);
                            x->pid = sideid(m, q);
                            paint(m, x->syb, x->sy1);
                            if (q->type == TURF) { x->pid = (short)(x->vf + 8); paint(m, x->sy0p, x->syb); }
                            if (x->sline) { x->pid = lineid(m); paint(m, x->sy0, (short)(x->sy0 + 1)); }
                        }
                    }
                }
                if (x->topf) {
                    const Seg *r = x->runstart;
                    short y0 = x->ty0, y1 = x->ty1;
                    x->pkey = (x->vf & 1) ? 0 : (short)(KTOP | r->top);
                    x->depth = x->df;
                    x->pid = (short)((r->type << 5) | x->vf);
                    if (y1 - y0 >= 3) {
                        paint(m, (short)(y0 + 1), y1);
                        x->pid = lineid(m);
                        paint(m, x->ty0, (short)(x->ty0 + 1));
                    } else
                        paint(m, y0, y1);
                    if (x->ty0 >= x->gend) goto bdone;
                }
                s = x->runend;
                if (!x->phase) { x->phase = 1; goto above; }
            }
        above:
            {
                const Seg *a2 = x->segk;
                for (;;) {
                    const Seg *p;
                    SB *sb;
                    if (a2 <= SEGP(x->seg0)) break;
                    a2--;
                    x->topf = 0; m->d4 = NONE; x->runstart = a2;
                    if (a2->flags & SEG_BOTEXP) {
                        short yf = PROJ(m->a1, (short)(a2->bot * 2), m->d7, m->d5), yn;
                        cachestore(m, (short)(a2->bot * 2), yf);
                        if (yf <= x->gstart) break;
                        x->ty0 = yf;
                        yn = near_y(m, (short)(a2->bot * 2));
                        m->d4 = yn;
                        if (yn < x->gend) { x->ty1 = yn; x->topf = 1; }
                    }
                    sb = sbuf;
                    p = a2;
                    if (x->rkey) {
                        for (;;) {
                            short y0, y1;
                            if (p < SEGP(x->seg0)) goto a1end;
                            if (p != x->runstart && (p->flags & SEG_BOTEXP)) goto a1end;
                            if (m->d4 == NONE) m->d4 = PROJ(m->a3, (short)(p->bot * 2), m->d6, m->d5);
                            if (m->d4 <= x->gstart) break;
                            y1 = m->d4;
                            y0 = m->d4 = PROJ(m->a3, (short)(p->top * 2), m->d6, m->d5);
                            if (y0 < x->gend) { sb->s = p; sb->y0 = y0; sb->y1 = y1; sb++; }
                            p--;
                        }
                    }
                    for (;;) {                       /* skip the rest of this run */
                        if (p < SEGP(x->seg0)) break;
                        if (p != x->runstart && (p->flags & SEG_BOTEXP)) break;
                        p--;
                    }
                a1end:
                    a2 = p + 1;
                    x->runend = a2;
                    if (sb != sbuf) {
                        sidekeys(m);
                        while (sb > sbuf) {
                            const Seg *q;
                            sb--;
                            q = sb->s;
                            x->sy0 = sb->y0; x->sy1 = sb->y1;
                            sidepieces(m, q);
                            x->pid = lineid(m);
                            paint(m, x->sy0, (short)(x->sy0 + 1));
                            if (q->type == TURF) { x->pid = (short)(x->vf + 8); paint(m, x->sy0p, x->syb); }
                            x->pid = sideid(m, q);
                            paint(m, x->syb, x->sy1);
                        }
                    }
                    if (x->topf) {
                        const Seg *r = x->runstart;
                        short yn = x->ty1, yf = x->ty0;
                        x->pkey = (x->vf & 1) ? 0 : (short)(KBOT | r->bot);
                        x->depth = x->df;
                        x->pid = (short)(((r->type << 5) | x->vf) + 12);
                        if (yf - yn >= 3) {
                            paint(m, yn, (short)(yf - 1));
                            x->pid = lineid(m);
                            paint(m, (short)(x->ty0 - 1), x->ty0);
                        } else
                            paint(m, yn, yf);
                    }
                    a2 = x->runend;
                }
                s = x->segk;
                if (s->top && m->a1[s->top] > 0) s++;
                goto brun;
            }
        bdone:
            x->pch0 = x->ch0; x->pch1 = x->ch1; x->pcy0 = x->cy0; x->pcy1 = x->cy1;
            x->ncache = x->nch;
            if (!x->ng) break;
            m->d6 = m->d7;
            m->a3 = m->a1;
            x->dn = x->df;
            x->vf ^= 16;
            if (x->df >= x->fogd) x->vf |= 1;
        }
        {
            short d1 = 0;
            if (!x->slow) linkfast(m, &d1);
            x->rstart[col] = d1;
        }
    }
}

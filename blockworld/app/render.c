/* Blockworld column renderer: C reference implementation of engine/render.py (bit-exact).
 * See render.h for the phases and engine/render.py for the specification. r_cast_asm (cast.s)
 * produces the same run lists as r_cast; both leave uncovered rows as R_GAPID runs, which
 * r_edges turns into the sky / haze / fog background. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "render.h"
#include "world.h"
#include "tables.h"

#define F 256
#define DMIN 40
#define DSPLIT 513
#define LMIN 3
#define HAZEH 24
#define KTOP 0x1000
#define KBOT 0x2000
#define KX 0x3000
#define KZ 0x4000
#define R_MAXD (48 * 256)          /* view distance + distant hills */
#define NFAR 8
#define FARSTEP (4 * 256)

short r_nc, r_h, r_dmax, r_h0;
RRun r_runs[R_MAXNC][R_MAXN];
short r_start[R_MAXNC];
short r_comp[R_MAXNC][R_MAXC * 2];
RStats r_stats;
RCtx r_ctx;
unsigned char r_shade[4];
static short r_pbuf[R_MAXP * 6];

static short DMAX, PW;
static short camx[R_MAXNC];
static short recip[R_MAXD + 1];
static unsigned char rsh[R_MAXD + 1];
static unsigned char fogtab[32];
/* the height tables t8 and t16 are followed by the type id table: TIDW + b holds (b & 31) << 5 |
   r_shade[b >> 5] for a type byte b (cast.s reads it at 112 bytes past either table) */
#define TIDW 56
_Static_assert(WY + 1 <= TIDW, "TIDW");
static short t8[TIDW + 128], t16[TIDW + 128], b8[WY + 1], b16[WY + 1];
static unsigned char r_pl[2 * BW_NIDS][8] __attribute__((aligned(4))), r_pr[2 * BW_NIDS][8] __attribute__((aligned(4)));
static unsigned short r_fill[2 * (R_MAXH + 1)];         /* the rasteriser's unrolled fill */
static Rast8Ctx r_q __attribute__((aligned(4)));         /* moving frames: its pattern table p16 */
#define r_p16 r_q.p16
static unsigned short r_pres[2 * (R_MAXNC / 4) + 8];     /* the present's generated loop */

/* the offsets cast.s uses (68000 build: 32-bit pointers, 2-byte alignment) */
#ifdef __m68k__
_Static_assert(offsetof(Rast8Ctx, p16) == 48 && offsetof(Rast8Ctx, j) == 24 && offsetof(Rast8Ctx, col) == 32 && offsetof(Rast8Ctx, sv) == 36
               && offsetof(Rast8Ctx, p2) == 32816, "Rast8Ctx");
#define CHECK(f, o) _Static_assert(offsetof(RCtx, f) == (o), #f)
CHECK(H, 0); CHECK(ng, 4); CHECK(nn, 6); CHECK(gstart, 8); CHECK(gend, 10); CHECK(depth, 12);
CHECK(dn, 14); CHECK(df, 16); CHECK(vf, 18); CHECK(ty1, 20); CHECK(fogd, 22); CHECK(dmax, 24);
CHECK(ncache, 26); CHECK(pch0, 28); CHECK(pcy0, 32); CHECK(nch, 36); CHECK(ch0, 38); CHECK(cy0, 42);
CHECK(skey, 46); CHECK(sface, 48); CHECK(pid, 50); CHECK(pkey, 52); CHECK(ty0, 54); CHECK(dirx, 56);
CHECK(plx, 60); CHECK(fx, 64); CHECK(kx0, 68); CHECK(ncols, 72); CHECK(col, 74); CHECK(var0, 76);
CHECK(pwx4, 80); CHECK(runs, 84); CHECK(t8, 88); CHECK(t16, 92); CHECK(b8, 96); CHECK(b16, 100);
CHECK(recip, 104); CHECK(camx, 108); CHECK(cp0, 112); CHECK(bnear, 116); CHECK(recend, 120);
CHECK(varc, 78); CHECK(seg0, 124); CHECK(segk, 128); CHECK(lastg, 132); CHECK(rkey, 136);
CHECK(mode, 138); CHECK(savesp, 140); CHECK(botlim, 144); CHECK(pbuf, 148); CHECK(topsp, 152); CHECK(botsp, 156); CHECK(slow, 160); CHECK(nnlim, 162);
CHECK(rstart, 164); CHECK(sbp, 168); CHECK(runstart, 172); CHECK(runend, 176); CHECK(topf, 180); CHECK(sy0, 182);
CHECK(sy1, 184); CHECK(sy0p, 186); CHECK(syb, 188); CHECK(sline, 190); CHECK(cstep, 192); CHECK(cfirst, 194); CHECK(nproj, 196);
CHECK(nrun, 200); CHECK(rdx, 204); CHECK(camxl, 208); CHECK(camzl, 212); CHECK(coarse, 216); CHECK(sbuf, 220);
CHECK(rec, 220 + 8 * R_MAXSB); CHECK(gaps0, 220 + 8 * R_MAXSB + 10 * R_MAXREC);
CHECK(gaps, 226 + 8 * R_MAXSB + 10 * R_MAXREC);
#endif
_Static_assert(sizeof(RRun) == 12, "RRun");

static short tab_nc, tab_h;
void r_set_shade(const unsigned char sh[4])
{
    short b;
    for (b = 0; b < 4; b++)
        r_shade[b] = sh[b];
    for (b = 0; b < 128; b++)
        t8[TIDW + b] = t16[TIDW + b] = (short)(((b & 31) << 5) | r_shade[b >> 5]);
}

/* n / d by the 68000's 32 / 16 divide (the quotient must fit 16 bits) */
static unsigned short udiv(unsigned long n, unsigned short d)
{
#ifdef __m68k__
    __asm__("divu.w %1,%0" : "+d"(n) : "dm"(d));
    return (unsigned short)n;
#else
    return (unsigned short)(n / d);
#endif
}

static short recip_dmax = -1, pat_done;
#define DSTEP 1763                         /* (above it the step loop beats a divide: q / d < 5) */
void r_config(short nc, short h, short dmax_blocks)
{
    short c;
    long d;
    r_nc = nc;
    r_h = h;
    r_dmax = dmax_blocks;
    r_set_shade(r_shade);
    DMAX = (short)(dmax_blocks * 256);
    PW = (short)(((long)nc * 4 * 8192) / F);
    for (c = 0; c < nc; c++) {
        long num = (long)(2 * c + 1 - nc) * 16384, q = num / nc;
        if (num % nc && num < 0) q--;                      /* floor division, as Python // */
        camx[c] = (short)q;
    }
    if (DMAX != recip_dmax) {          /* (the same values as long divisions) */
        long q = 0, r = 0, dn = DMAX + NFAR * FARSTEP;
        recip_dmax = DMAX;
        for (d = 0; d < DSPLIT; d++) {
            recip[d] = (short)udiv((unsigned long)F << 12, (unsigned short)(d > DMIN ? d : DMIN));
            rsh[d] = 12;
        }
        {                                  /* divisions up to DSTEP, then F << 16 = q d + r, */
            short *rp = recip + DSPLIT, dd;    /*  0 <= r < d, stepped from the last d's */
            for (dd = DSPLIT; dd < DSTEP && dd <= dn; dd++)
                *rp++ = (short)udiv((unsigned long)F << 16, (unsigned short)dd);
            q = udiv((unsigned long)F << 16, (unsigned short)(DSTEP - 1));
            r = ((long)F << 16) - q * (DSTEP - 1);
            for (; dd <= dn; dd++) {
                r -= q;
                while (r < 0) { q--; r += dd; }
                *rp++ = (short)q;
            }
            memset(rsh + DSPLIT, 16, dn - DSPLIT + 1);
        }
    }
    for (c = 0; c <= dmax_blocks; c++)
        fogtab[c] = c >= dmax_blocks - 3;
    if (!pat_done) {                    /* the pattern tables (once: r_set_pat changes them after) */
        short id, p;
        pat_done = 1;
        for (id = 0; id < BW_NIDS; id++) {             /* each pattern without and with the edge pixel */
            uint32_t a, b;                      /* (native order: every mask is the same in each byte) */
            memcpy(&a, bw_pat[id], 4);
            memcpy(&b, bw_pat[id] + 4, 4);
            uint32_t *pl = (uint32_t *)r_pl[id], *pr = (uint32_t *)r_pr[id];
            uint32_t *q = (uint32_t *)r_p16[id];
            uint32_t *ple = (uint32_t *)r_pl[id | R_EDGE], *pre = (uint32_t *)r_pr[id | R_EDGE];
            uint32_t *qe = (uint32_t *)r_p16[id | R_EDGE];
            unsigned char *q2 = r_q.p2[id], *q2e = r_q.p2[id | R_EDGE];
            pl[0] = a & 0xF0F0F0F0U; pl[1] = b & 0xF0F0F0F0U;
            pr[0] = a & 0x0F0F0F0FU; pr[1] = b & 0x0F0F0F0FU;
            ple[0] = pl[0] | 0x80808080U; ple[1] = pl[1] | 0x80808080U;
            pre[0] = pr[0] | 0x08080808U; pre[1] = pr[1] | 0x08080808U;
            q[0] = q[2] = a; q[1] = q[3] = b;
            qe[0] = qe[2] = a | 0x80808080U; qe[1] = qe[3] = b | 0x80808080U;
            for (p = 0; p < 8; p++) {
                unsigned char v = bw_pat[id][p];
#ifdef __m68k__
                unsigned short w = (unsigned short)(v * 0x101), we = (unsigned short)(w | 0x8000);
                ((unsigned short *)q2)[p] = ((unsigned short *)q2)[p + 8] = w;
                ((unsigned short *)q2e)[p] = ((unsigned short *)q2e)[p + 8] = we;   /* (raster16: */
#else                                                                               /*  the right */
                q2[2 * p] = q2[2 * p + 16] = q2[2 * p + 1] = q2[2 * p + 17] = v;    /*  byte has */
                q2e[2 * p] = q2e[2 * p + 16] = (unsigned char)(v | 0x80);           /*  no edge) */
                q2e[2 * p + 1] = q2e[2 * p + 17] = v;
#endif
            }
        }
    }
    if (nc != tab_nc || h != tab_h) {   /* the rasteriser's fills (not for a new distance) */
        static const unsigned short reg[8] = {0, 1, 2, 4, 5, 6, 7, 3};
        short r;
        tab_nc = nc; tab_h = h;
        for (r = 0; r < h; r++) {
            r_fill[2 * r] = (unsigned short)(0x1140 | reg[r & 7]);     /* move.b dN,d16(a0) */
            r_fill[2 * r + 1] = (unsigned short)(2 * r);                 /* column-pair layout */
        }
        {   /* the present: per row, a word from each column pair, then the next row */
            short k, n = 0, pairs = nc / 4;
            for (k = 0; k < pairs; k++) {
                r_pres[n++] = 0x30E9;                                   /* move.w d16(a1),(a0)+ */
                r_pres[n++] = (unsigned short)(k * 2 * h);
            }
            r_pres[n++] = 0x5489;                                       /* addq.l #2,a1 */
            r_pres[n++] = 0xD0C1;                                       /* adda.w d1,a0 */
            r_pres[n] = 0x51C8;                                         /* dbra d0,top */
            r_pres[n + 1] = (unsigned short)(-2 * (n + 1));
            n += 2;
            r_pres[n] = 0x4E75;
        }
        r_fill[2 * h] = 0x4E75;                                         /* rts */
        r_fill[2 * h + 1] = 0x4E71;
    }
}

/* ---- per-column painting (C reference) ---------------------------------------------------- */

static short H, H0;
static RRun *R;                               /* the current column's runs */
static short nn;                              /* next free run (byte offset) */
static short ng;                              /* gaps */
static short gy0[R_MAXG], gy1[R_MAXG], gnode[R_MAXG];

#define RUN(off) ((RRun *)((char *)R + (off)))

static void r_paint(short y0, short y1, short id, short key, short d)
{
    short g;
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    if (y0 >= y1) return;
    r_stats.pieces++;
    for (g = 0; g < ng;) {
        short a = gy0[g], b = gy1[g], c0, c1, k;
        RRun *n;
        if (b <= y0) { g++; continue; }
        if (a >= y1) break;
        if (nn > (R_MAXN - 2) * 12 || ng >= R_MAXG - 1) return;   /* out of room: drop (never seen) */
        c0 = a > y0 ? a : y0;
        c1 = b < y1 ? b : y1;
        n = RUN(gnode[g]);
        if (c0 == a) {
            if (c1 == b) {                                   /* fills the whole gap */
                n->id = id; n->key = key; n->d = d;
                for (k = g; k < ng - 1; k++) {
                    gy0[k] = gy0[k + 1]; gy1[k] = gy1[k + 1]; gnode[k] = gnode[k + 1];
                }
                ng--;
                continue;
            }
            {                                                /* top part of the gap */
                RRun *m = RUN(nn);
                m->yend = b; m->id = R_GAPID; m->key = 0; m->d = R_DFAR; m->next = n->next;
                n->yend = c1; n->id = id; n->key = key; n->d = d; n->next = nn;
                gy0[g] = c1; gnode[g] = nn;
                nn += 12;
            }
            return;                                          /* c1 == y1 */
        }
        if (c1 == b) {                                       /* bottom part of the gap */
            RRun *m = RUN(nn);
            m->yend = b; m->id = id; m->key = key; m->d = d; m->next = n->next;
            n->yend = c0; n->next = nn;
            gy1[g] = c0;
            nn += 12;
            g++;
            continue;
        }
        {                                                    /* middle: gap, piece, gap */
            RRun *m1 = RUN(nn), *m2 = RUN(nn + 12);
            m2->yend = b; m2->id = R_GAPID; m2->key = 0; m2->d = R_DFAR; m2->next = n->next;
            m1->yend = c1; m1->id = id; m1->key = key; m1->d = d; m1->next = nn + 12;
            n->yend = c0; n->next = nn;
            for (k = ng; k > g + 1; k--) {
                gy0[k] = gy0[k - 1]; gy1[k] = gy1[k - 1]; gnode[k] = gnode[k - 1];
            }
            gy0[g + 1] = c1; gy1[g + 1] = b; gnode[g + 1] = nn + 12;
            gy1[g] = c0;
            ng++;
            nn += 24;
            return;
        }
    }
}

/* Y = H0 - (hr * r >> s), clamped to +-16383 (outside the screen either way) */
static short proj(short hr, short r, short s)
{
    long p = ((long)hr * r) >> s;
    r_stats.projs++;
    if (p > 16383) p = 16383;
    if (p < -16383) p = -16383;
    return (short)(H0 - p);
}

/* near-edge projection cache: far projections of the previous cell (same distance) */
static short cache_h[2], cache_y[2], pcache_h[2], pcache_y[2], ncache;

static short near_y(short h, short hr, short rn, short sn)
{
    short i;
    for (i = 0; i < ncache; i++)
        if (pcache_h[i] == h) return pcache_y[i];
    return proj(hr, rn, sn);
}

static void frame_setup(const RCam *cam, short *hrel)
{
    short h;
    H = r_h;
    r_h0 = H0 = (short)(r_h / 2 + cam->pitch);
    for (h = 0; h <= WY; h++) {
        long v = ((long)h << 8) - cam->y, w = v * 16, vb = v - 64, wb = vb * 16;
        hrel[h] = (short)v;
        t8[h] = (short)v;
        t16[h] = (short)(w > 32767 ? 32767 : w < -32767 ? -32767 : w);
        b8[h] = (short)vb;
        b16[h] = (short)(wb > 32767 ? 32767 : wb < -32767 ? -32767 : wb);
    }
}

typedef struct CellCtx {
    short rf, sf, rn, sn, df, dn, fog, var, keyon, face, sidekey, newc;
    short psky, pcov;               /* the previous cell: its first top, the shade class under it */
    const short *hrel;
} CellCtx;

/* light: a segment's class is in its type byte's bits 5-6 (0 open sky, 1 under a tree, 2 covered,
   3 torch-lit); the frame's shade (dither) for each class is OR'ed into the id's fog field */
#define TYP(b) ((b) & 31)
#define CLS(b) (((b) >> 5) & 3)
#define TOPSH(x, s) ((x)->fog | r_shade[CLS((s)->type)])         /* top face: its own class */
#define BOTSH(x, s) ((x)->fog | r_shade[CLS((s)[1].type)])       /* bottom: the next segment's */
/* a side: the light of the cell in front of it */
static short side_sh(const CellCtx *x, const Seg *s)
{
    short k = CLS(s->type) == 3 ? 3 : s->top > x->psky ? 0 : x->pcov;
    return (short)(x->fog | r_shade[k]);
}

#define GSTART (ng ? gy0[0] : 32767)
#define GEND (ng ? gy1[ng - 1] : -32768)

/* segments [s, end) at and below the eye (end 0: to the list's end), top-down */
static void cell_below(CellCtx *x, const Seg *s, const Seg *end)
{
    short chain = 0, chainok = 0, hidden = 0, yf, yn, y0, y1, kk;
    const short *hrel = x->hrel;
    for (; s->top && s != end; s++) {
        short tr = hrel[s->top], br = hrel[s->bot], t = TYP(s->type);
        short base = (short)((t << 5) + x->var), sh = TOPSH(x, s);
        r_stats.segs++;
        if (s->flags & SEG_TOPEXP) {                  /* run top */
            hidden = 0; chainok = 0;
            if (tr <= 0) {
                yf = proj(tr, x->rf, x->sf);
                if (x->newc < 2) { cache_h[x->newc] = s->top; cache_y[x->newc] = yf; x->newc++; }
                if (yf >= GEND) break;
                if (tr < 0) {                         /* top face */
                    yn = near_y(s->top, tr, x->rn, x->sn);
                    chain = yn; chainok = 1;
                    if (yn > GSTART) {
                        kk = x->keyon ? (KTOP | (s->top << 1)) : 0;
                        if (yn - yf >= LMIN)
                            r_paint(yf, yf + 1, BW_LINE + sh, kk, x->df);
                        r_paint(yf, yn, base + sh, kk, x->df);
                    }
                }
            }
        } else if (!x->face)
            continue;
        if (x->face && !hidden) {
            short ss = side_sh(x, s), sid = base + ss + (x->face == 1 ? 4 : 8);
            kk = x->keyon ? x->sidekey : 0;
            y0 = chainok ? chain : proj(tr, x->rn, x->sn);
            if (y0 >= GEND) { hidden = 1; chainok = 0; continue; }
            y1 = proj(br, x->rn, x->sn);
            chain = y1; chainok = 1;
            if (y1 > GSTART) {
                if (tr > 0)
                    r_paint(y0, y0 + 1, BW_LINE + ss, kk, x->dn);
                if (t == TURF) {
                    short yb = proj(tr - 64, x->rn, x->sn);
                    r_paint(y0, yb, (x->var ? BW_TURFBAND_B : BW_TURFBAND) + ss, kk, x->dn);
                }
                r_paint(y0, y1, sid, kk, x->dn);
            }
        }
    }
}

/* segments [s0, k) entirely above the eye, bottom-up */
static void cell_above(CellCtx *x, const Seg *s0, const Seg *k)
{
    short chain = 0, chainok = 0, hidden = 0, yf, yn, y0, y1, kk;
    const short *hrel = x->hrel;
    const Seg *s;
    for (s = k - 1; s >= s0; s--) {
        short tr = hrel[s->top], br = hrel[s->bot], t = TYP(s->type);
        short base = (short)((t << 5) + x->var);
        r_stats.segs++;
        if (s->flags & SEG_BOTEXP) {                  /* run bottom: bottom face */
            short sb = BOTSH(x, s);
            hidden = 0;
            yf = proj(br, x->rf, x->sf);
            if (x->newc < 2) { cache_h[x->newc] = s->bot; cache_y[x->newc] = yf; x->newc++; }
            if (yf <= GSTART) break;
            yn = near_y(s->bot, br, x->rn, x->sn);
            chain = yn; chainok = 1;
            if (yn < GEND) {
                kk = x->keyon ? (KBOT | (s->bot << 1)) : 0;
                if (yf - yn >= LMIN)
                    r_paint(yf - 1, yf, BW_LINE + sb, kk, x->df);
                r_paint(yn, yf, base + sb + 12, kk, x->df);
            }
        } else if (!x->face)
            continue;
        if (x->face && !hidden) {
            short ss = side_sh(x, s), sid = base + ss + (x->face == 1 ? 4 : 8);
            kk = x->keyon ? x->sidekey : 0;
            y1 = chainok ? chain : proj(br, x->rn, x->sn);
            if (y1 <= GSTART) { hidden = 1; chainok = 0; continue; }
            y0 = proj(tr, x->rn, x->sn);
            chain = y0; chainok = 1;
            if (y0 < GEND) {
                r_paint(y0, y0 + 1, BW_LINE + ss, kk, x->dn);
                if (t == TURF) {
                    short yb = proj(tr - 64, x->rn, x->sn);
                    r_paint(y0, yb, (x->var ? BW_TURFBAND_B : BW_TURFBAND) + ss, kk, x->dn);
                }
                r_paint(y0, y1, sid, kk, x->dn);
            }
        }
    }
}


#ifdef CELL_MODEL
int getenv_model;
/* Model of the 68000 general cell path: the cell's pieces top-down into a sorted, non-overlapping
 * list (a run's bottom face and its line yield to the pieces after them; anything else wins over
 * later pieces), then painted: the tail attached to the last gap's end bottom-up, the head
 * attached to the first gap's start top-down, the rest in any order. Same result as cell_above
 * + cell_below. */
typedef struct { short y0, y1, id, key, d, yields; } MPiece;
static MPiece mp[R_MAXSB * 4 + 8];
static short mnp;
static void madd(short y0, short y1, short id, short key, short d, short yields)
{
    while (mnp && mp[mnp - 1].y1 > y0) {
        MPiece *q = &mp[mnp - 1];
        if (q->yields) {
            q->y1 = y0;
            if (q->y0 >= q->y1) { mnp--; continue; }
        } else
            y0 = q->y1;
        break;
    }
    if (y0 >= y1) return;
    if (y1 <= GSTART) return;                     /* above every gap */
    mp[mnp].y0 = y0; mp[mnp].y1 = y1; mp[mnp].id = id; mp[mnp].key = key; mp[mnp].d = d;
    mp[mnp].yields = yields;
    mnp++;
}
static void cell_model(CellCtx *x, const Seg *s)
{
    const short *hrel = x->hrel;
    short ge = GEND, i, j;
    mnp = 0;
    while (s->top) {
        const Seg *r0 = s, *r1 = s;
        short tr, br, chain = 0, chainok = 0, kk, yf, yn, y0, y1;
        while (!(r1->flags & SEG_BOTEXP) && r1[1].top) r1++;
        tr = hrel[r0->top]; br = hrel[r1->bot];
        if (tr < 0) {                                  /* top face */
            short sh = TOPSH(x, r0), base = (short)((TYP(r0->type) << 5) + x->var + sh);
            yf = proj(tr, x->rf, x->sf);
            if (x->newc < 2) { cache_h[x->newc] = r0->top; cache_y[x->newc] = yf; x->newc++; }
            if (yf >= ge) break;
            yn = near_y(r0->top, tr, x->rn, x->sn);
            chain = yn; chainok = 1;
            kk = x->keyon ? (KTOP | (r0->top << 1)) : 0;
            if (yn - yf >= LMIN) {
                madd(yf, yf + 1, BW_LINE + sh, kk, x->df, 0);
                madd(yf + 1, yn, base, kk, x->df, 0);
            } else
                madd(yf, yn, base, kk, x->df, 0);
        }
        if (x->face) {                                 /* sides */
            for (s = r0; s <= r1; s++) {
                short t = s->top, str = hrel[t], sbr = hrel[s->bot];
                short ss = side_sh(x, s), base = (short)((TYP(s->type) << 5) + x->var + ss), y0p;
                kk = x->keyon ? x->sidekey : 0;
                y0 = chainok ? chain : proj(str, x->rn, x->sn);
                if (y0 >= ge) goto paint;
                y1 = proj(sbr, x->rn, x->sn);
                chain = y1; chainok = 1;
                y0p = y0;
                if (str > 0) { madd(y0, y0 + 1, BW_LINE + ss, kk, x->dn, 0); y0p = y0 + 1; }
                if (TYP(s->type) == TURF) {
                    short yb = proj(str - 64, x->rn, x->sn);
                    if (yb < y0p) yb = y0p;
                    madd(y0p, yb, (x->var ? BW_TURFBAND_B : BW_TURFBAND) + ss, kk, x->dn, 0);
                    y0p = yb;
                }
                madd(y0p, y1, base + (x->face == 1 ? 4 : 8), kk, x->dn, 0);
            }
        }
        if (br > 0 && (r1->flags & SEG_BOTEXP)) {      /* bottom face */
            short sb = BOTSH(x, r1), base = (short)((TYP(r1->type) << 5) + x->var + sb);
            yn = chainok ? chain : proj(br, x->rn, x->sn);
            yf = proj(br, x->rf, x->sf);
            if (x->newc < 2) { cache_h[x->newc] = r1->bot; cache_y[x->newc] = yf; x->newc++; }
            if (yn >= ge) break;
            kk = x->keyon ? (KBOT | (r1->bot << 1)) : 0;
            if (yf - yn >= LMIN) {
                madd(yn, yf - 1, base + 12, kk, x->df, 1);
                madd(yf - 1, yf, BW_LINE + sb, kk, x->df, 1);
            } else
                madd(yn, yf, base + 12, kk, x->df, 1);
        }
        s = r1 + 1;
    }
paint:
    /* the tail attached to the gap end (bottom-up), the head attached to the gap start, the rest */
    for (i = mnp - 1; i >= 0 && mp[i].y1 >= GEND && mp[i].y0 < GEND; i--)
        r_paint(mp[i].y0, mp[i].y1, mp[i].id, mp[i].key, mp[i].d);
    for (j = 0; j <= i && mp[j].y0 <= GSTART; j++)
        r_paint(mp[j].y0, mp[j].y1, mp[j].id, mp[j].key, mp[j].d);
    for (; j <= i; j++)
        r_paint(mp[j].y0, mp[j].y1, mp[j].id, mp[j].key, mp[j].d);
}
#endif

/* per-frame values shared by the C caster's columns */
static short f_hrel[WY + 1], f_dirx, f_dirz, f_plx, f_plz, f_mx0, f_mz0, f_fx, f_fz;
static long f_camx, f_camz, f_eye;

static void cast_setup(const RCam *cam)
{
    frame_setup(cam, f_hrel);
    f_dirx = bw_sin[(cam->yaw + 256) & 1023];
    f_dirz = bw_sin[cam->yaw & 1023];
    f_plx = (short)(((long)-bw_sin[cam->yaw & 1023] * PW) >> 14);
    f_plz = (short)(((long)bw_sin[(cam->yaw + 256) & 1023] * PW) >> 14);
    f_mx0 = (short)(cam->x >> 8); f_mz0 = (short)(cam->z >> 8);
    f_fx = (short)(cam->x & 255); f_fz = (short)(cam->z & 255);
    f_camx = cam->x; f_camz = cam->z; f_eye = cam->y;
}

/* distant hills beyond the view distance: coarse heights sampled along the ray */
static void far_hills(short rdx, short rdz)
{
    short k, stx = (short)(((long)rdx * FARSTEP) >> 14), stz = (short)(((long)rdz * FARSTEP) >> 14);
    long px = f_camx + (((long)rdx * DMAX) >> 14), pz = f_camz + (((long)rdz * DMAX) >> 14);
    if (!ng) return;
    for (k = 1; k <= NFAR; k++) {
        short d = (short)(DMAX + k * FARSTEP), h, y;
        long hx, hz;
        px += stx; pz += stz;
        hx = px >> 10; hz = pz >> 10;
        h = (hx >= 0 && hx < CWX && hz >= 0 && hz < CWX) ? bw_coarse[hx * CWX + hz] : SEA + 1;
        y = proj((short)(((long)h << 8) - f_eye), recip[d], rsh[d]);
        r_paint(y, y + 1, BW_FARLINE, 0, d);
        r_paint(y, H, BW_FARHILL + (k > NFAR / 2), 0, d);
    }
}

/* one column with the C reference caster */
static void cast_col_c(short c)
{
    const short *hrel = f_hrel;
    short rdx = (short)(f_dirx + (((long)f_plx * camx[c]) >> 14));
    short rdz = (short)(f_dirz + (((long)f_plz * camx[c]) >> 14));
    short adx = rdx < 0 ? -rdx : rdx, adz = rdz < 0 ? -rdz : rdz;
    long ddx = adx < 128 ? 0x7FFF : (1L << 22) / adx;
    long ddz = adz < 128 ? 0x7FFF : (1L << 22) / adz;
    long sdx, sdz;
    short sx, sz, mx, mz, dn, face, rn, sn, xplane, zplane, psky, pcov;
    long pstep;
    Seg **cp;
    if (ddx > 0x7FFF) ddx = 0x7FFF;
    if (ddz > 0x7FFF) ddz = 0x7FFF;
    if (rdx > 0) { sx = 1; sdx = ((256 - f_fx) * ddx) >> 8; } else { sx = -1; sdx = (f_fx * ddx) >> 8; }
    if (rdz > 0) { sz = 1; sdz = ((256 - f_fz) * ddz) >> 8; } else { sz = -1; sdz = (f_fz * ddz) >> 8; }
    R = r_runs[c];
    r_start[c] = 0;
    R[0].yend = H; R[0].id = R_GAPID; R[0].key = 0; R[0].d = R_DFAR; R[0].next = -1;
    nn = 12;
    ng = 1; gy0[0] = 0; gy1[0] = H; gnode[0] = 0;
    mx = f_mx0; mz = f_mz0;
    cp = &bw_cols[(long)(mx + BORDER) * PWX + (mz + BORDER)];
    pstep = sx * (long)PWX;
    dn = 0; face = 0;
    rn = recip[0]; sn = rsh[0];
    ncache = 0;
    psky = 0; pcov = 2;
    xplane = KX + 32 + (sx < 0);
    zplane = KZ + 32 + (sz < 0);
    for (;;) {
        short df, nxt, rf, sf, fog, var, keyon, last;
        if (sdx < sdz) { df = (short)sdx; nxt = 1; } else { df = (short)sdz; nxt = 2; }
        last = df >= DMAX;
        if (last) df = DMAX;
        rf = recip[df]; sf = rsh[df];
        fog = fogtab[dn >> 8];
        var = ((mx ^ mz) & 1) << 4;
        keyon = fog < 1;
        r_stats.cells++;
        {
            const Seg *s0 = *cp, *k = s0;
            CellCtx cc;
            cc.rf = rf; cc.sf = sf; cc.rn = rn; cc.sn = sn; cc.df = df; cc.dn = dn;
            cc.fog = fog; cc.var = var; cc.keyon = keyon; cc.face = face;
            cc.sidekey = face == 1 ? (short)(xplane + mx) : (short)(zplane + mz);
            cc.hrel = hrel; cc.newc = 0; cc.psky = psky; cc.pcov = pcov;
#ifdef CELL_MODEL
            if (getenv_model) { cell_model(&cc, s0); goto celldone; }
#endif
            while (k->top && hrel[k->bot] > 0) k++;          /* first segment not above the eye */
            if (k->top && hrel[k->top] > 0) {                 /* across the eye level: first */
                cell_below(&cc, k, k + 1);
                k++;
            }
            cell_above(&cc, s0, k);
            cell_below(&cc, k, 0);
#ifdef CELL_MODEL
        celldone:
#endif
            /* the far projections of this cell are the next cell's near ones */
            ncache = cc.newc;
            psky = s0->top;
            pcov = s0->top && (TYP(s0->type) == LEAVES || TYP(s0->type) == LOG) ? 1 : 2;
            pcache_h[0] = cache_h[0]; pcache_y[0] = cache_y[0];
            pcache_h[1] = cache_h[1]; pcache_y[1] = cache_y[1];
        }
        if (last || ng == 0) break;
        if (nxt == 1) { mx += sx; sdx += ddx; cp += pstep; }
        else { mz += sz; sdz += ddz; cp += sz; }
        face = nxt;
        dn = df; rn = rf; sn = sf;
    }
    far_hills(rdx, rdz);
}

void r_cast_cols(const RCam *cam, short first, short step)
{
    short c;
    cast_setup(cam);
    memset(&r_stats, 0, sizeof r_stats);
    for (c = first; c < r_nc; c += step)
        cast_col_c(c);
}

void r_cast(const RCam *cam) { r_cast_cols(cam, 0, 1); }

/* ---- the 68000 caster: per-frame setup, then cast.s -------------------------------------- */

void r_cast_asm(const RCam *cam) { r_cast_cols_asm(cam, 0, 1); }

void r_cast_cols_asm(const RCam *cam, short first, short step)
{
    short hrel[WY + 1];
    RCtx *x = &r_ctx;
    short mx0 = (short)(cam->x >> 8), mz0 = (short)(cam->z >> 8);
    frame_setup(cam, hrel);
    x->H = r_h;
    x->H0 = r_h0;
    x->dirx = bw_sin[(cam->yaw + 256) & 1023];
    x->dirz = bw_sin[cam->yaw & 1023];
    x->plx = (short)(((long)-bw_sin[cam->yaw & 1023] * PW) >> 14);
    x->plz = (short)(((long)bw_sin[(cam->yaw + 256) & 1023] * PW) >> 14);
    x->fx = (short)(cam->x & 255);
    x->fz = (short)(cam->z & 255);
    x->kx0 = (short)(KX + 32 + mx0);
    x->kz0 = (short)(KZ + 32 + mz0);
    x->var0 = (short)(((mx0 ^ mz0) & 1) << 4);
    x->ncols = r_nc;
    x->dmax = DMAX;
    x->fogd = (short)((r_dmax - 3) * 256);
    x->pwx4 = (long)PWX * 4;
    x->runs = &r_runs[0][0];
    x->t8 = t8; x->t16 = t16; x->b8 = b8; x->b16 = b16;
    x->recip = recip;
    x->camx = camx;
    x->cp0 = &bw_cols[(long)(mx0 + BORDER) * PWX + (mz0 + BORDER)];
    x->gaps0[0] = -32768; x->gaps0[1] = -32768; x->gaps0[2] = 0;
    x->rstart = r_start;
    x->camxl = cam->x; x->camzl = cam->z;
    x->coarse = bw_coarse;
    x->pbuf = r_pbuf;
    x->cfirst = first; x->cstep = step;
    cast_columns(x);
}

/* ---- edges and background ----------------------------------------------------------------- */

void r_edges(void)
{
    short c, hz = r_h0 - HAZEH;
    for (c = 0; c < r_nc; c++) {
        const RRun *ra = r_runs[c], *rb = c ? r_runs[c - 1] : 0;
        const RRun *a = (const RRun *)((const char *)ra + r_start[c]);
        const RRun *b = rb ? (const RRun *)((const char *)rb + r_start[c - 1]) : 0;
        short y = 0, n = 0, *out = r_comp[c];
        while (y < r_h) {
            short ea = a->yend, eb = b ? b->yend : r_h, e = ea < eb ? ea : eb;
            short id = a->id;
            if (id == R_GAPID) {                     /* background by row band */
                if (y < hz) { id = BW_SKY; if (e > hz) e = hz; }
                else if (y < r_h0) { id = BW_HAZE; if (e > r_h0) e = r_h0; }
                else id = BW_FOGBG;
            }
            if (b && a->key != b->key)
                id |= R_EDGE;
            if (n && out[2 * n - 1] == id)
                out[2 * n - 2] = e;
            else if (n < R_MAXC) {
                out[2 * n] = e;
                out[2 * n + 1] = id;
                n++;
            }
            y = e;
            if (ea == e) a = (const RRun *)((const char *)ra + a->next);
            if (b && eb == e) b = (const RRun *)((const char *)rb + b->next);
        }
    }
}

void r_raster(unsigned char *buf, short rowbytes)
{
    short k;
    for (k = 0; k < r_nc / 2; k++) {
        const short *pa = r_comp[2 * k], *pb = r_comp[2 * k + 1];
        unsigned char *col = buf + (long)(k >> 1) * 2 * r_h + (k & 1);
        short y = 0;
        (void)rowbytes;
        while (y < r_h) {
            short e = pa[0] < pb[0] ? pa[0] : pb[0], p;
            const unsigned char *L = bw_pat[pa[1] & (BW_NIDS - 1)], *Q = bw_pat[pb[1] & (BW_NIDS - 1)];
            unsigned char v[8], el = (pa[1] & R_EDGE) ? 0x80 : 0, er = (pb[1] & R_EDGE) ? 0x08 : 0;
            for (p = 0; p < 8; p++)
                v[p] = (unsigned char)((L[p] & 0xF0) | (Q[p] & 0x0F) | el | er);
            for (; y < e; y++)
                col[2 * y] = v[y & 7];
            if (pa[0] == e) pa += 2;
            if (pb[0] == e) pb += 2;
        }
    }
}

void r_frame(const RCam *cam, unsigned char *buf, short rowbytes)
{
    r_cast(cam);
    r_edges();
    r_raster(buf, rowbytes);
}

/* ---- the 68000 edge pass and rasteriser (raster.s) ------------------------------------------ */

/* moving frames: column 2j drawn 8 px wide at byte j (its pattern byte), the uncovered rows by
 * band, the leftmost pixel black where its key differs from column 2j - 2's */
void r_raster8(unsigned char *buf, short rowbytes)
{
    short j, hz = r_h0 - HAZEH;
    for (j = 0; 2 * j < r_nc; j++) {
        short c = 2 * j, y = 0;
        const RRun *ra = r_runs[c], *rb = j ? r_runs[c - 2] : 0;
        const RRun *a = (const RRun *)((const char *)ra + r_start[c]);
        const RRun *b = rb ? (const RRun *)((const char *)rb + r_start[c - 2]) : 0;
        unsigned char *col = buf + (long)(j >> 1) * 2 * r_h + (j & 1);
        (void)rowbytes;
        while (y < r_h) {
            short ea = a->yend, eb = b ? b->yend : r_h, e = ea < eb ? ea : eb;
            short id = a->id;
            const unsigned char *P;
            if (id == R_GAPID) {
                if (y < hz) { id = BW_SKY; if (e > hz) e = hz; }
                else if (y < r_h0) { id = BW_HAZE; if (e > r_h0) e = r_h0; }
                else id = BW_FOGBG;
            }
            if (b && a->key != b->key)
                id |= R_EDGE;
            P = r_p16[id];
            for (; y < e; y++)
                col[2 * y] = P[y & 7];
            if (ea == e) a = (const RRun *)((const char *)ra + a->next);
            if (b && eb == e) b = (const RRun *)((const char *)rb + b->next);
        }
    }
}

/* Full size's moving frames: column 4j drawn 16 px wide as the word column j (left byte: its pattern
 * byte with the edge pixel, right byte: the pattern byte), edges against column 4j - 4 */
void r_raster16(unsigned char *buf)
{
    short j, hz = r_h0 - HAZEH;
    for (j = 0; 4 * j < r_nc; j++) {
        short c = 4 * j, y = 0;
        const RRun *ra = r_runs[c], *rb = j ? r_runs[c - 4] : 0;
        const RRun *a = (const RRun *)((const char *)ra + r_start[c]);
        const RRun *b = rb ? (const RRun *)((const char *)rb + r_start[c - 4]) : 0;
        unsigned char *col = buf + (long)j * 2 * r_h;
        while (y < r_h) {
            short ea = a->yend, eb = b ? b->yend : r_h, e = ea < eb ? ea : eb;
            short id = a->id;
            const unsigned char *P, *Q;
            if (id == R_GAPID) {
                if (y < hz) { id = BW_SKY; if (e > hz) e = hz; }
                else if (y < r_h0) { id = BW_HAZE; if (e > r_h0) e = r_h0; }
                else id = BW_FOGBG;
            }
            Q = r_p16[id];
            if (b && a->key != b->key)
                id |= R_EDGE;
            P = r_p16[id];
            for (; y < e; y++) {
                col[2 * y] = P[y & 7];
                col[2 * y + 1] = Q[y & 7];
            }
            if (ea == e) a = (const RRun *)((const char *)ra + a->next);
            if (b && eb == e) b = (const RRun *)((const char *)rb + b->next);
        }
    }
}

void r_raster16_asm(unsigned char *buf)
{
    Rast8Ctx *q = &r_q;
    q->runs = &r_runs[0][0]; q->start = r_start; q->buf = buf; q->code = 0;
    q->nc = r_nc; q->h = r_h; q->hz = (short)(r_h0 - HAZEH); q->h0 = r_h0;
    raster16_asm(q);
}

void r_raster8_asm(unsigned char *buf, short rowbytes)
{
    Rast8Ctx *q = &r_q;
    q->runs = &r_runs[0][0]; q->start = r_start; q->buf = buf; q->code = 0;
    q->nc = r_nc; q->h = r_h; q->hz = (short)(r_h0 - HAZEH); q->h0 = r_h0;
    (void)rowbytes;
    raster8_asm(q);
}

void r_edges_asm(void)
{
    EdgeCtx e;
    e.runs = &r_runs[0][0]; e.start = r_start; e.comp = &r_comp[0][0];
    e.nc = r_nc; e.h = r_h; e.hz = (short)(r_h0 - HAZEH); e.h0 = r_h0;
    edges_asm(&e);
}

void r_raster_asm(unsigned char *buf, short rowbytes)
{
    RastCtx x;
    x.comp = &r_comp[0][0]; x.buf = buf; x.pl = &r_pl[0][0]; x.pr = &r_pr[0][0]; x.code = r_fill;
    x.nb = r_nc / 2; x.rb = 2; x.h = r_h;          /* column-pair layout: 2 bytes per row */
    (void)rowbytes;
    raster_asm(&x);
}

/* the frame buffer (column pairs: byte b of row y at (b >> 1) * 2 * H + 2 * y + (b & 1)) to the
 * screen, h rows of nc / 8 bytes from dst */
void r_present(unsigned char *dst, short dstRowBytes, const unsigned char *buf)
{
    present_asm(r_pres, buf, dst, (long)(r_h - 1), (long)(dstRowBytes - r_nc / 2));
}

/* the same as plain rows (rb = nc / 2 bytes each), for tests */
void r_rows(const unsigned char *buf, unsigned char *rows)
{
    short y, b, rb = r_nc / 2;
    for (y = 0; y < r_h; y++)
        for (b = 0; b < rb; b++)
            rows[(long)y * rb + b] = buf[(long)(b >> 1) * 2 * r_h + 2 * y + (b & 1)];
}

void r_set_pat(short id, const unsigned char pat[8])
{
    short e, p;
    for (e = 0; e < 2; e++) {
        short i = (short)(id | (e ? R_EDGE : 0));
        for (p = 0; p < 16; p++) {
            unsigned char v = (unsigned char)(pat[p & 7] | (e ? 0x80 : 0));
            r_p16[i][p] = v;
            r_q.p2[i][2 * p] = v;
            r_q.p2[i][2 * p + 1] = pat[p & 7];
            if (p < 8) {
                r_pl[i][p] = (unsigned char)((pat[p] & 0xF0) | (e ? 0x80 : 0));
                r_pr[i][p] = (unsigned char)((pat[p] & 0x0F) | (e ? 0x08 : 0));
            }
        }
    }
}

void r_set_sky(const unsigned char sky[8], const unsigned char haze[8], const unsigned char fog[8])
{
    r_set_pat(BW_SKY, sky);
    r_set_pat(BW_HAZE, haze);
    r_set_pat(BW_FOGBG, fog);
}

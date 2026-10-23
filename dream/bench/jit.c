/* Dream: generate 68000 code for one layer from its weights.
 * Exactly the code of dream/engine/jit.py (the spec; its encodings are checked against GNU as).
 * One call of the generated code handles one grid position: a0 = input base, a1 = output base,
 * a2 = bias vector, a3 = table middle; d0-d7 scratch; a0 and a1 are restored on return.
 * Written for speed on the 68000: no 32-bit multiply/divide in the loops (offsets are precomputed
 * once per layer, rows are walked with pointers). */
#include "jit.h"

#define W(x) (*p++ = (unsigned short)(x))
#define MAXIO 2048

static short offin[MAXIO], offout[MAXIO];
static int jtwo;                                /* two-term gains: k holds (k1, e2) per channel */

void jit_set_two(int two) { jtwo = two; }

static unsigned short *shifts(unsigned short *p, int n, int k)
{
    while (k > 0) { int s = k > 8 ? 8 : k; W(0xE040 | (s & 7) << 9 | n); k -= s; }
    while (k < 0) { int s = -k > 8 ? 8 : -k; W(0xE140 | (s & 7) << 9 | n); k += s; }
    return p;
}

/* the gain part of the requantisation for accumulator dn, channel codes kc */
static unsigned short *gain(unsigned short *p, int n, const signed char *kc)
{
    if (jtwo && kc[1]) {
        int e2 = kc[1];
        W(0x3000 | n);                                  /* move.w dn,d0 */
        p = shifts(p, 0, (e2 < 0 ? -e2 : e2) - 4);      /* d0 = acc >> k2 */
        p = shifts(p, n, kc[0]);                        /* dn = acc >> k1 */
        W((e2 > 0 ? 0xD040 : 0x9040) | n << 9);         /* add.w/sub.w d0,dn */
    } else {
        p = shifts(p, n, kc[0]);
    }
    return p;
}

static unsigned short *rqx(unsigned short *p, int n, const signed char *kc, int mode, int clamp, short lo, short hi);

static unsigned short *shift_rq(unsigned short *p, int n, const signed char *kc, int mode)
{
    p = gain(p, n, kc);
    W(0xD05A | n << 9);                         /* add.w (a2)+,dn */
    if (mode == M_ADDH) W(0xD051 | n << 9);     /* add.w (a1),dn */
    if (mode == M_RAW) {
        W(0x32C0 | n);                          /* move.w dn,(a1)+ */
    } else {
        W(0x48C0 | n);                          /* ext.l dn */
        W(0xD080 | n << 9 | n);                 /* add.l dn,dn */
        W(0x32F3); W(n << 12 | 0x0800);         /* move.w 0(a3,dn.l),(a1)+: 65536-entry table */
    }
    return p;
}

/* byte offsets of a gathered input (2x2 space-to-depth) or scattered output (depth-to-space) */
static void offsets(short *dst, int n, int gathered, short Wt, short Ct)
{
    int i = 0, q, c;
    if (!gathered) {
        for (i = 0; i < n; i++) dst[i] = (short)(i * 2);
        return;
    }
    for (q = 0; q < 4; q++) {
        short base = (short)(((q >> 1) * (Wt + 2) + (q & 1)) * Ct * 2);
        for (c = 0; c < Ct; c++) dst[i++] = (short)(base + c * 2);
    }
}

unsigned short *jit_pw_ex(unsigned short *p, const LDesc *l, const signed char *w, const signed char *k,
                          short Ws, short Cs, short Wd, short Cd, int clamp, short lo, short hi);

unsigned short *jit_pw(unsigned short *p, const LDesc *l, const signed char *w, const signed char *k,
                       short Ws, short Cs, short Wd, short Cd)
{
    return jit_pw_ex(p, l, w, k, Ws, Cs, Wd, Cd, 0, 0, 0);
}

unsigned short *jit_pw_ex(unsigned short *p, const LDesc *l, const signed char *w, const signed char *k,
                          short Ws, short Cs, short Wd, short Cd, int clamp, short lo, short hi)
{
    int cin = l->cin, cout = l->cout, o0, i, j;
    int q4 = l->wq, csz = q4 ? 6 : 7;
    short ocur = 0;
    const signed char *row[7];
    offsets(offin, cin, l->gather, Ws, Cs);
    offsets(offout, cout, l->scatter, Wd, Cd);
    row[0] = w;
    for (o0 = 0; o0 < cout; o0 += csz) {
        int m = cout - o0 < csz ? cout - o0 : csz;
        short cur = 0;
        for (j = 1; j < m; j++) row[j] = row[j - 1] + cin;
        for (j = 0; j < m; j++) W(0x7000 | (j + 1) << 9);           /* moveq #0,dj */
        for (i = 0; i < cin; i++) {
            int any = 0, big = 0;
            for (j = 0; j < m; j++) {
                int v = row[j][i];
                any |= v;
                if (v >= 2 || v <= -2) big = 1;
            }
            if (!any) continue;
            if (offin[i] != cur) { W(0x41E8); W(offin[i] - cur); cur = offin[i]; }  /* lea d(a0),a0 */
            W(0x3018); cur += 2;                                      /* move.w (a0)+,d0 */
            if (q4) {
                if (big) { W(0x3E00); W(0xDE47); }                    /* move.w d0,d7; add.w d7,d7 */
                for (j = 0; j < m; j++) {
                    int v = row[j][i], a = v < 0 ? -v : v;
                    unsigned short r7 = (v > 0 ? 0xD047 : 0x9047) | (j + 1) << 9;
                    for (; a >= 2; a -= 2) W(r7);                     /* add/sub.w d7,dj */
                    if (a) W((v > 0 ? 0xD040 : 0x9040) | (j + 1) << 9);
                }
            } else {
                for (j = 0; j < m; j++) {
                    int v = row[j][i];
                    if (v > 0) W(0xD040 | (j + 1) << 9);              /* add.w d0,dj */
                    else if (v < 0) W(0x9040 | (j + 1) << 9);         /* sub.w d0,dj */
                }
            }
        }
        if (cur) { W(0x41E8); W(-cur); }
        for (j = 0; j < m; j++) {
            short off = offout[o0 + j];
            if (off != ocur) { W(0x43E9); W(off - ocur); ocur = off; }   /* lea d(a1),a1 */
            p = rqx(p, j + 1, k + (o0 + j) * (jtwo ? 2 : 1), l->mode, clamp, lo, hi);
            ocur += 2;
        }
        row[0] = row[m - 1] + cin;
    }
    if (ocur) { W(0x43E9); W(-ocur); }
    W(0x4E75);
    return p;
}

static unsigned short *clamp_rq(unsigned short *p, int n, const signed char *kc, int mode, short lo, short hi)
{
    p = gain(p, n, kc);
    W(0xD05A | n << 9);                         /* add.w (a2)+,dn */
    if (mode == M_ADDH) W(0xD051 | n << 9);     /* add.w (a1),dn */
    if (mode == M_RAW) { W(0x32C0 | n); return p; }   /* move.w dn,(a1)+ */
    W(0x0C40 | n); W(lo); W(0x6C06);            /* cmpi.w #lo,dn; bge.s +6 */
    W(0x303C | n << 9); W(lo); W(0x600A);       /* move.w #lo,dn; bra.s +10 */
    W(0x0C40 | n); W(hi); W(0x6F04);            /* cmpi.w #hi,dn; ble.s +4 */
    W(0x303C | n << 9); W(hi);                  /* move.w #hi,dn */
    W(0x32C0 | n);                              /* move.w dn,(a1)+ */
    return p;
}

static unsigned short *rqx(unsigned short *p, int n, const signed char *kc, int mode, int clamp, short lo, short hi)
{
    return clamp ? clamp_rq(p, n, kc, mode, lo, hi) : shift_rq(p, n, kc, mode);
}

static unsigned short *jit_dw_any(unsigned short *p, const LDesc *l, const signed char *w, const signed char *k,
                                  short Ws, short C, int clamp, short lo, short hi);

unsigned short *jit_dw(unsigned short *p, const LDesc *l, const signed char *w, const signed char *k,
                       short Ws, short C)
{
    return jit_dw_any(p, l, w, k, Ws, C, 0, 0, 0);
}

unsigned short *jit_dw_clamp(unsigned short *p, const LDesc *l, const signed char *w, const signed char *k,
                             short Ws, short C, short lo, short hi)
{
    return jit_dw_any(p, l, w, k, Ws, C, 1, lo, hi);
}

static unsigned short *jit_dw_any(unsigned short *p, const LDesc *l, const signed char *w, const signed char *k,
                                  short Ws, short C, int clamp, short lo, short hi)
{
    int c, t, ntap = l->center ? 1 : 9;
    short tapoff[9];
    const signed char *wc = w;
    for (t = 0; t < ntap; t++) {
        int dy = l->center ? 0 : t / 3 - 1, dx = l->center ? 0 : t % 3 - 1;
        tapoff[t] = (short)((dy * (Ws + 2) + dx) * C * 2);
    }
    for (c = 0; c < C; c++, wc += ntap) {
        int first = 1, firstnz = -1;
        for (t = 0; t < ntap; t++) if (wc[t]) { firstnz = t; break; }
        if (firstnz < 0 || wc[firstnz] < 0) { W(0x7200); first = 0; }   /* moveq #0,d1 */
        for (t = 0; t < ntap; t++) {
            short d;
            if (!wc[t]) continue;
            d = (short)(tapoff[t] + c * 2);
            if (first) {
                if (d == 0) W(0x3210); else { W(0x3228); W(d); }      /* move.w d(a0),d1 */
                first = 0;
            } else if (d == 0) {
                W(wc[t] > 0 ? 0xD250 : 0x9250);                        /* add/sub.w (a0),d1 */
            } else {
                W(wc[t] > 0 ? 0xD268 : 0x9268); W(d);                  /* add/sub.w d(a0),d1 */
            }
        }
        p = rqx(p, 1, k + c * (jtwo ? 2 : 1), l->mode, clamp, lo, hi);
    }
    W(0x43E9); W(-2 * C);
    W(0x4E75);
    return p;
}

/* ------------------------------------------------------------------ chunk-code compiler (model files v2)
 * codes: for each chunk of 7 output rows, for each input i, code = sum_j (w[o0+j][i] + 1) 3^j.
 * Each code's add/sub instructions come from a template built once (2187 x up to 7 words), so the
 * compiler copies words instead of looping over weights. Output identical to jit_pw (ternary). */
#define NCODE 2187
#define ZERO7 1093
static unsigned short *tmpl;            /* NCODE x 8: count, then the words */

int jit_templates(void *(*alloc)(long))
{
    short d[7] = {0, 0, 0, 0, 0, 0, 0};
    int c, j;
    if (tmpl) return 1;
    tmpl = (unsigned short *)alloc((long)NCODE * 16);
    if (!tmpl) return 0;
    for (c = 0; c < NCODE; c++) {
        unsigned short *t = tmpl + c * 8, n = 0;
        for (j = 0; j < 7; j++) {
            if (d[j] == 2) t[1 + n++] = (unsigned short)(0xD040 | (j + 1) << 9);       /* add.w d0,dj */
            else if (d[j] == 0) t[1 + n++] = (unsigned short)(0x9040 | (j + 1) << 9);  /* sub.w d0,dj */
        }
        t[0] = n;
        for (j = 0; j < 7 && ++d[j] == 3; j++) d[j] = 0;                             /* next code */
    }
    return 1;
}

void jit_free_templates(void (*fr)(void *))
{
    if (tmpl) fr(tmpl);
    tmpl = 0;
}

unsigned short *jit_pw_codes(unsigned short *p, const LDesc *l, const unsigned short *codes,
                             const signed char *k, short Ws, short Cs, short Wd, short Cd)
{
    const int clamp = 0;
    const short lo = 0, hi = 0;
    int cin = l->cin, cout = l->cout, o0, i, j;
    short ocur = 0;
    offsets(offin, cin, l->gather, Ws, Cs);
    offsets(offout, cout, l->scatter, Wd, Cd);
    for (o0 = 0; o0 < cout; o0 += 7) {
        int m = cout - o0 < 7 ? cout - o0 : 7;
        short cur = 0;
        const short *oi = offin;
        for (j = 0; j < m; j++) W(0x7000 | (j + 1) << 9);           /* moveq #0,dj */
        for (i = 0; i < cin; i++, oi++) {
            unsigned short c = *codes++;
            const unsigned short *t;
            short n;
            if (c == ZERO7) continue;
            if (*oi != cur) { W(0x41E8); W(*oi - cur); cur = *oi; }   /* lea d(a0),a0 */
            W(0x3018); cur += 2;                                      /* move.w (a0)+,d0 */
            t = tmpl + c * 8;
            n = (short)*t++;
            while (n-- > 0) *p++ = *t++;
        }
        if (cur) { W(0x41E8); W(-cur); }
        for (j = 0; j < m; j++) {
            short off = offout[o0 + j];
            if (off != ocur) { W(0x43E9); W(off - ocur); ocur = off; }   /* lea d(a1),a1 */
            p = rqx(p, j + 1, k + (o0 + j) * (jtwo ? 2 : 1), l->mode, clamp, lo, hi);
            ocur += 2;
        }
    }
    if (ocur) { W(0x43E9); W(-ocur); }
    W(0x4E75);
    return p;
}

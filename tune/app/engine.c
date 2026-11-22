/*
 * Tune's integer GRU step and sampler: the arithmetic of tune/ref/tuneref.py (Gen.feed, Gen.logits,
 * Gen.sample), in C for the 68000 with the hot loops in kernels68k.s. See tune/ref/SPEC.md.
 */
#include "engine.h"

#define TCLAMP (32767 / (TM_NFEAT + 1))

/* ---- model blob ------------------------------------------------------------------------------ */

/* Fill m from the header words w (native byte order); pointers are into base. Returns the blob's
   size in words, or -1. Layout: tune/ref/export.py write_bin. */
static long parse(const short *w, TuneModel *m, const short *base)
{
    short i, l, n, L;
    long hw, bw, H0;
    L = w[3]; m->V = w[4]; n = w[5];
    if (n != TM_NTAB || L < 1 || L > TM_MAXL)
        return -1;
    m->L = L;
    hw = 6;
    for (i = 0; i < n; i++)
        m->rows[i] = w[hw + i];
    hw += n;
    for (l = 0; l < L; l++) {
        TMLayer *y = &m->l[l];
        y->H = w[hw]; y->G = w[hw + 1]; y->k_hh = w[hw + 2];
        hw += 3;
        if (l > 0) {
            y->Gi = w[hw]; y->k_ih = w[hw + 1];
            hw += 2;
        }
    }
    m->Go = w[hw]; m->k_out = w[hw + 1];
    hw += 2;
    H0 = m->l[0].H;
    bw = hw;
    for (i = 0; i < n; i++) {
        m->tab[i] = base + bw;
        bw += (long)m->rows[i] * 3 * H0;
    }
    for (l = 0; l < L; l++) {
        TMLayer *y = &m->l[l];
        long H = y->H;
        y->Mb = base + bw; bw += 4 * H;
        y->codes_hh = base + bw; bw += 3 * H * y->G;
        if (l > 0) {
            y->b_ih = base + bw; bw += 3 * H;
            y->M_ih = base + bw; bw += 3 * H;
            y->codes_ih = base + bw; bw += 3 * H * y->Gi;
        }
    }
    m->B_out = base + bw; bw += m->V;
    m->M_out = base + bw; bw += m->V;
    m->codes_out = base + bw; bw += (long)m->V * m->Go;
    return bw;
}

void tm_swap(void *blob, long len)
{
    /* host only: the blob is big-endian; swap every word after the magic */
    unsigned char *p = (unsigned char *)blob, t;
    long k;
    for (k = 4; k + 1 < len; k += 2) {
        t = p[k]; p[k] = p[k + 1]; p[k + 1] = t;
    }
}

short tm_load(TuneModel *m, const void *blob, long len)
{
    const char *c = (const char *)blob;
    const short *w = (const short *)blob;
    long words;
    if (c[0] != 'T' || c[1] != 'U' || c[2] != 'N' || c[3] != 'E' || w[2] != 2)
        return -1;
    words = parse(w, m, w);
    if (words < 0 || m->V != TK_VOCAB)
        return -2;
    if (words * 2 > len)
        return -3;
    return 0;
}

/* ---- generator ------------------------------------------------------------------------------- */

static long h3max(const TuneModel *m)
{
    long n = m->V;
    short l;
    for (l = 0; l < m->L; l++)
        if (3L * m->l[l].H > n)
            n = 3L * m->l[l].H;
    return n;
}

long tg_bytes(const TuneModel *m)
{
    long n = 0;
    short l;
    for (l = 0; l < m->L; l++)
        n += m->l[l].H + 5L * m->l[l].G + 243L * m->l[l].G;
    n += 6L * m->l[0].H + 2 * h3max(m) + 2L * m->V;
    return 2 * n + 16;
}

void tg_init(TuneGen *s, const TuneModel *m, void *work)
{
    short *p = (short *)work;
    long H3max = h3max(m);
    short l;
    s->m = m;
    s->cond = p; p += 3L * m->l[0].H;
    s->x = p; p += H3max;
    s->acc = p; p += H3max;
    for (l = 0; l < m->L; l++) {
        s->h[l] = p; p += m->l[l].H;
        s->hq[l] = p; p += 5L * m->l[l].G;
        s->lut[l] = p; p += 243L * m->l[l].G;
    }
    s->allow = p; p += m->V;
    s->lg = p; p += m->V;
    s->zero = p;
    for (l = 0; l < 3 * m->l[0].H; l++)
        s->zero[l] = 0;
    s->invT = 256;
    s->cut = 1152;            /* 4.5 nats (Q8): tokens much less likely than the best are never picked */
    s->maxtok = 600;
    s->maxbars = 40;
}

void tg_start(TuneGen *s, short type, short meter, short mode, uint32_t seed)
{
    const TuneModel *m = s->m;
    short H3 = 3 * m->l[0].H, i, l;
    const short *a = m->tab[TM_NFEAT] + (long)type * H3, *b = m->tab[TM_NFEAT + 1] + (long)meter * H3,
                *c = m->tab[TM_NFEAT + 2] + (long)mode * H3;
    for (i = 0; i < H3; i++) {
        short v = a[i] + b[i] + c[i];       /* |each| <= TCLAMP: no overflow */
        s->cond[i] = v > TCLAMP ? TCLAMP : v < -TCLAMP ? -TCLAMP : v;
    }
    for (l = 0; l < m->L; l++) {
        for (i = 0; i < m->l[l].H; i++)
            s->h[l][i] = 0;
        for (i = 0; i < 5 * m->l[l].G; i++)
            s->hq[l][i] = 0;
        k_tab5(s->hq[l], m->l[l].G, s->lut[l]);
    }
    gram_init(&s->g, (short)(tk_meters[meter][0] * 48 / tk_meters[meter][1]));
    s->rng = seed;
    tg_feed(s, TK_BOS);
}

#define LUTMID(p) ((const short *)((const char *)(p) + 32768L))

void tg_feed(TuneGen *s, short tok)
{
    const TuneModel *m = s->m;
    short H3 = 3 * m->l[0].H, i, l, f[TM_NFEAT];
    const short *r[TM_NFEAT];
    short *x = s->x;

    if (tok != TK_BOS)
        gram_step(&s->g, tok);
    tok_inputs(tok, &f[0], &f[1]);
    gram_feats(&s->g, f + 2);
    gram_lookback(&s->g, f + 6);
    for (i = 0; i < TM_NFEAT; i++)
        r[i] = m->tab[i] + (long)f[i] * H3;
    /* the input rows (never overflow: each entry is within +-TCLAMP) */
    k_sum5(x, H3, s->cond, r[0], r[1], r[2], r[3], 0);
    k_sum5(x, H3, r[4], r[5], r[6], r[7], r[8], 1);
    k_sum5(x, H3, r[9], s->zero, s->zero, s->zero, s->zero, 1);
    for (l = 0; l < m->L; l++) {
        const TMLayer *y = &m->l[l];
        short H = y->H, *hq = s->hq[l], *h = s->h[l];
        if (l > 0) {
            k_mv(y->codes_ih, LUTMID(s->lut[l - 1]), 3L * H, s->acc, y->Gi);
            k_scale(s->acc, y->M_ih, y->k_ih, y->b_ih, x, 3L * H);
        }
        k_mv(y->codes_hh, LUTMID(s->lut[l]), 3L * H, s->acc, y->G);
        k_gru(x, s->acc, y->Mb, y->k_hh, h, H, tk_sig + 4096, tk_tanh + 4096);
        for (i = 0; i < H; i++)
            hq[i] = (short)((h[i] + 64) >> 7);
        k_tab5(hq, y->G, s->lut[l]);
    }
}

short tg_sample(TuneGen *s)
{
    const TuneModel *m = s->m;
    short V = m->V, v, n, j, *acc = s->acc, *al = s->allow, *lg = s->lg, k = m->k_out;
    short invT = s->invT, cut = s->cut, lmax;
    const short *M = m->M_out, *B = m->B_out, *lm = LUTMID(s->lut[m->L - 1]);
    unsigned char *ok = s->ok;
    unsigned short u, P, r, run;

    gram_allowed(&s->g, ok, s->maxtok, s->maxbars);
    /* output rows only for runs of allowed tokens */
    n = 0;
    for (v = 0; v < V; ) {
        short e;
        if (!ok[v]) {
            v++;
            continue;
        }
        for (e = v; e < V && ok[e]; e++)
            al[n++] = e;
        k_mv(m->codes_out + (long)v * m->Go, lm, e - v, acc + v, m->Go);
        v = e;
    }
    s->nallow = n;
    if (n == 1)
        return al[0];
    lmax = -32768;
    for (j = 0; j < n; j++) {
        short g, L;
        long t;
        v = al[j];
        g = (short)((((long)acc[v] * M[v]) >> 16) << k);   /* wraps like asl.w */
        t = (long)g + B[v];
        L = t > 32767 ? 32767 : t < -32768 ? -32768 : (short)t;
        if (invT != 256) {
            t = ((long)L * invT) >> 8;
            L = t > 32767 ? 32767 : t < -32768 ? -32768 : (short)t;
        }
        lg[j] = L;
        if (L > lmax)
            lmax = L;
    }
    P = 0;
    for (j = 0; j < n; j++) {
        long d = (long)lmax - lg[j];
        lg[j] = d <= cut ? (short)tk_exp[d >> 3] : 0;
        P += lg[j];
    }
    s->rng = s->rng * (uint32_t)1103515245UL + (uint32_t)12345UL;
    u = (unsigned short)(s->rng >> 16);
    r = (unsigned short)(((uint32_t)u * P) >> 16);
    run = 0;
    for (j = 0; j < n; j++) {
        run += lg[j];
        if (r < run)
            return al[j];
    }
    return TK_EOS;   /* not reached */
}

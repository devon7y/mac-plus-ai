/* Pixel inference engine. Every function mirrors llm/ref/model.py (see SPEC.md sections in
 * comments); results must be bit-identical. */
#include "engine.h"
#include <string.h>

#define I32MAX 2147483647L
#define I32MIN (-2147483647L - 1)

/* 16x16 multiplies and 32/16 division as single 68000 instructions (GCC otherwise calls libgcc) */
#if defined(__m68k__) || defined(__mc68000__)
static inline int32_t MULS(int16_t a, int16_t b)
{
    int32_t r = a;
    __asm__("muls.w %1,%0" : "+d"(r) : "dm"(b));
    return r;
}
static inline uint32_t MULU(uint16_t a, uint16_t b)
{
    uint32_t r = a;
    __asm__("mulu.w %1,%0" : "+d"(r) : "dm"(b));
    return r;
}
static inline uint16_t DIVU(uint32_t n, uint16_t d)   /* quotient must fit 16 bits */
{
    __asm__("divu.w %1,%0" : "+d"(n) : "dm"(d));
    return (uint16_t)n;
}
#else
#define MULS(a, b) ((int32_t)(int16_t)(a) * (int32_t)(int16_t)(b))
#define MULU(a, b) ((uint32_t)(uint16_t)(a) * (uint32_t)(uint16_t)(b))
#define DIVU(n, d) ((uint16_t)((uint32_t)(n) / (uint16_t)(d)))
#endif

uint32_t (*px_clock)(void);
uint32_t px_prof[PP_N];
uint32_t px_lprof[PL_N];
#ifdef PX_PROFILE
#define PB(ph) uint32_t pt_##ph = px_clock ? px_clock() : 0
#define PE(ph, idx) do { if (px_clock) px_prof[idx] += px_clock() - pt_##ph; } while (0)
#else
#define PB(ph) do { } while (0)
#define PE(ph, idx) do { } while (0)
#endif

/* ------------------------------------------------------------------ scale floats (SPEC 3) */
static const SF ZERO = {0, 0};
static const SF SQRT_D = {25905, -11};
static const SF ATT_C = {20724, -11};

static int bitlen(uint32_t v)
{
    int b = 0;
    if (v >> 16) { b = 16; v >>= 16; }
    if (v >> 8) { b += 8; v >>= 8; }
    while (v) { b++; v >>= 1; }
    return b;
}

static SF sf_norm(uint32_t v, int e)
{
    SF r;
    int b;
    if (!v) return ZERO;
    b = bitlen(v);
    if (b > 15) { v >>= (b - 15); e += b - 15; }
    else if (b < 15) { v <<= (15 - b); e -= 15 - b; }
    r.m = (int16_t)v;
    r.e = (int16_t)e;
    return r;
}

static SF sf_int(uint32_t v) { return sf_norm(v, 0); }
static SF sf_pow2(int k) { SF r; r.m = 16384; r.e = (int16_t)(k - 14); return r; }

static inline SF sf_mul(SF a, SF b)     /* product in [2^28, 2^30): shift 14 or 15 (= sf_norm) */
{
    SF r;
    uint32_t p;
    if (!a.m || !b.m) return ZERO;
    p = MULU(a.m, b.m);
    if (p & 0x20000000UL) { r.m = (int16_t)(p >> 15); r.e = (int16_t)(a.e + b.e + 15); }
    else { r.m = (int16_t)(p >> 14); r.e = (int16_t)(a.e + b.e + 14); }
    return r;
}

static inline SF sf_div(SF a, SF b)     /* quotient in (2^14, 2^16): shift 0 or 1 (= sf_norm) */
{
    SF r;
    uint16_t q;
    if (!a.m || !b.m) return ZERO;
    q = DIVU((uint32_t)(uint16_t)a.m << 15, (uint16_t)b.m);
    if (q & 0x8000) { r.m = (int16_t)(q >> 1); r.e = (int16_t)(a.e - b.e - 14); }
    else { r.m = (int16_t)q; r.e = (int16_t)(a.e - b.e - 15); }
    return r;
}

static uint32_t isqrt32(uint32_t v)
{
    uint32_t r = 0, bit = 1UL << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

static SF sf_sqrt(uint32_t v)
{
    int b, s;
    uint32_t vv;
    if (!v) return ZERO;
    b = bitlen(v);
    s = ((b - 30) & 1) ? b - 29 : b - 30;
    vv = s >= 0 ? v >> s : v << -s;
    return sf_norm(isqrt32(vv), s / 2);
}

static int sf_less(SF a, SF b)
{
    if (!a.m) return b.m != 0;
    if (!b.m) return 0;
    return a.e < b.e || (a.e == b.e && a.m < b.m);
}

/* round(x * f), x a 16-bit value; saturates to int32 when f.e >= 0 (never in practice) */
static inline int32_t sf_apply(int32_t x, SF f)
{
    int32_t p;
    int k;
    if (!f.m) return 0;
    p = MULS((int16_t)x, f.m);
    if (f.e >= 0) {
        if (p == 0) return 0;
        if (f.e >= 31) return p > 0 ? I32MAX : I32MIN;
        if (p > (I32MAX >> f.e)) return I32MAX;
        if (p < (I32MIN >> f.e)) return I32MIN;
        return (int32_t)((uint32_t)p << f.e);
    }
    k = -f.e;
    if (k >= 31) return 0;
    return (p + ((int32_t)1 << (k - 1))) >> k;
}

/* y[i] = round(x[i] * f) for results known to fit 16 bits (quantizers) */
static void apply16(const int16_t *x, int16_t *y, int n, SF f)
{
    int i, k = -f.e;
    int32_t half;
    if (!f.m || k >= 31) { memset(y, 0, n * 2); return; }
    if (k <= 0) { for (i = 0; i < n; i++) y[i] = (int16_t)sf_apply(x[i], f); return; }
    half = (int32_t)1 << (k - 1);
    for (i = 0; i < n; i++) y[i] = (int16_t)((MULS(x[i], f.m) + half) >> k);
}

static int32_t sat_add(int32_t a, int32_t b)
{
    int32_t s = (int32_t)((uint32_t)a + (uint32_t)b);
    if (a >= 0 && b >= 0 && s < 0) return I32MAX;
    if (a < 0 && b < 0 && s >= 0) return I32MIN;
    return s;
}

/* r[i] = sat32(r[i] + sat32(round(x[i] * f))) */
static void add_res(int32_t *r, const int16_t *x, SF f)
{
    int i, k = -f.e;
    int32_t half;
    if (!f.m || k >= 31) return;
    if (k <= 0) { for (i = 0; i < PX_D; i++) r[i] = sat_add(r[i], sf_apply(x[i], f)); return; }
    (void)half;
    k_addres(r, x, PX_D, f.m, k);
}

static int16_t clamp16(int32_t v, int lo, int hi) { return (int16_t)(v < lo ? lo : v > hi ? hi : v); }

static int exp_index(int32_t d) { return d >= 1022 ? 511 : (int)((d + 1) >> 1); }

/* ------------------------------------------------------------------ quantizers (SPEC 5.1) */
static SF quant16m(const int16_t *x, int n, int Q, int16_t *y, uint32_t M)   /* M = max|x| known */
{
    SF F;
    if (!M) { memset(y, 0, n * 2); return ZERO; }
    F = sf_div(sf_int(Q), sf_int(M));
    if (F.e < 0 && F.e > -31) k_apply16(x, y, n, F.m, -F.e);
    else apply16(x, y, n, F);
    return F;
}

static SF quant16(const int16_t *x, int n, int Q, int16_t *y)
{
    uint32_t M = k_maxabs16(x, n);
    SF F;
    if (!M) { memset(y, 0, n * 2); return ZERO; }
    F = sf_div(sf_int(Q), sf_int(M));
    if (F.e < 0 && F.e > -31) k_apply16(x, y, n, F.m, -F.e);
    else apply16(x, y, n, F);
    return F;
}

static SF normq_(const int32_t *r, const int16_t *g, SF gs, int Q, int16_t *y);
static SF normq(const int32_t *r, const int16_t *g, SF gs, int Q, int16_t *y)
{
    SF s;
    PB(n);
    s = normq_(r, g, gs, Q, y);
    PE(n, PP_NORM);
    return s;
}

static SF normq_(const int32_t *r, const int16_t *g, SF gs, int Q, int16_t *y)
{
    static int16_t t[PX_D];
    static int32_t a[PX_D];
    uint32_t M0, ss;
    unsigned long Ma;
    int sh, s2;
    SF F, num, den;
    M0 = k_maxabs32(r, PX_D);
    if (!M0) { memset(y, 0, PX_D * 2); return ZERO; }
    sh = bitlen(M0) - 15;
    ss = k_normpass(r, g, t, a, PX_D, sh, &Ma);     /* t = r >> sh, ss = sum (t*t)>>8, a = t*g */
    if (!Ma) { memset(y, 0, PX_D * 2); return ZERO; }
    s2 = bitlen(Ma) - 15;
    if (s2 < 0) s2 = 0;
    F = quant16m(t, PX_D, Q, y, k_shrmax(a, t, PX_D, s2));
    num = sf_mul(sf_mul(sf_pow2(s2 - 4), gs), SQRT_D);
    den = sf_mul(F, sf_sqrt(ss));
    return sf_div(num, den);
}

/* ------------------------------------------------------------------ ternary matvecs */
static int16_t xpad[520];   /* B5 inputs padded with zeros */

static void tern(PxModel *m, void *w, const int16_t *y, int n, int rows, int16_t *out)
{
    PB(t);
    if (m->variant == PX_W4) {
        k_tab4(y, n / 4, m->lut);
        k_mvw40((const int16_t *)w, m->lut + 16384, rows, out);       /* n = 160: 40 groups */
    }
#ifndef PX_NO_B5
    else {
        k_tab5(y, n / 5, m->lut);
        k_mvb5((const uint8_t *)w, m->lut, rows, n / 5, out);
    }
#endif
    PE(t, PP_TERN);
}

/* down projection: two kernel calls per row (SPEC 4), sum in 32 bits */
static void tern_down(PxModel *m, PxLayer *ly, int e, const int16_t *yh, int32_t *acc)
{
    int i;
    int16_t *t = m->tmp;
    PB(t);
    if (m->variant == PX_W4) {
        k_tab4(yh, 128, m->lut);
        k_mvw((const int16_t *)ly->wd[e], m->lut + 16384, 2 * PX_D, 64, t);
        for (i = 0; i < PX_D; i++) acc[i] = (int32_t)t[2 * i] + t[2 * i + 1];
    }
#ifndef PX_NO_B5
    else {
        memcpy(xpad, yh, PX_FF * 2);
        k_tab5(xpad, 103, m->lut);
        k_mvb5((const uint8_t *)ly->wd[e], m->lut, PX_D, 51, t);
        k_mvb5((const uint8_t *)ly->wd2[e], m->lut + 51 * 243, PX_D, 52, t + PX_D);
        for (i = 0; i < PX_D; i++) acc[i] = (int32_t)t[i] + t[PX_D + i];
    }
#endif
    PE(t, PP_TERN);
}

/* ------------------------------------------------------------------ attention (SPEC 5.2) */
static void attention(PxModel *m, PxLayer *ly, int slot, int nvalid)
{
    static int16_t y1[PX_D], acc[PX_QKVR], kc[PX_HD], qh[PX_HD], S[PX_W], z[PX_W], u[PX_W];
    static int16_t acco[PX_H][PX_HD], yo[PX_D], ao[PX_D];
    static int16_t p[PX_W];
    static uint32_t w[PX_W];
    SF b1, sq0, sk0, sv0, so[PX_H], Mo, bo, F;
    int i, j, h, s;
    b1 = normq(m->r, ly->g_attn, ly->gs_attn, 127, y1);
    tern(m, ly->qkv, y1, PX_D, PX_QKVR, acc);
    sq0 = sf_mul(ly->a_q, b1);
    sk0 = sf_mul(ly->a_k, b1);
    sv0 = sf_mul(ly->a_v, b1);
    PB(q);
    for (j = 0; j < PX_KVH; j++) {
        int16_t *kp = ly->kpc + (j * PX_W + slot) * PX_HD;
        uint8_t *vp = ly->vb + (j * PX_W + slot) * PX_HD;
        F = quant16(acc + PX_D + j * PX_HD, PX_HD, 7, kc);
        ly->sk[j][slot] = sf_div(sk0, F);
        for (i = 0; i < PX_HD; i++) kp[i] = (int16_t)(i * 32 + (kc[i] + 8) * 2 - 32768);   /* per-dim codes */
        F = quant16(acc + PX_D + PX_KVH * PX_HD + j * PX_HD, PX_HD, 7, kc);
        ly->sv[j][slot] = sf_div(sv0, F);
        for (i = 0; i < PX_HD; i++) vp[i] = (uint8_t)(2 * (kc[i] + 8));
    }
    PE(q, PP_AQ);
    for (h = 0; h < PX_H; h++) {
        SF Cq;
        int32_t zmax = -32768, P = 0, emax = -32768, Ws = 0;
        int sh, any = 0;
        j = h / (PX_H / PX_KVH);
        F = quant16(acc + h * PX_HD, PX_HD, 117, qh);
        Cq = sf_mul(sf_div(sq0, F), ATT_C);
        {
            PB(k);
            k_ptab(qh, PX_HD, m->ptabs);              /* 40 tables of 16 words: (c-8)*q_d */
            k_mvw40(ly->kpc + j * PX_W * PX_HD, m->ptabs + 16384, nvalid, S);
            PE(k, PP_ATTK);
        }
        PB(z);
        zmax = k_scores(S, (const int16_t *)ly->sk[j], nvalid, Cq.m, Cq.e, m->alibi[h], m->pos, z);
        PE(z, PP_AZ);
        PB(sm);
        P = k_softp(z, nvalid, zmax, px_exp12, p);
        for (s = 0; s < nvalid; s++)
            if (ly->sv[j][s].m) {
                if (!any || ly->sv[j][s].e > emax) emax = ly->sv[j][s].e;
                any = 1;
            }
        if (!any) { memset(acco[h], 0, sizeof acco[h]); so[h] = ZERO; PE(sm, PP_AS); continue; }
        Ws = (int32_t)k_softw(p, (const int16_t *)ly->sv[j], nvalid, emax, w);
        if (!Ws) { memset(acco[h], 0, sizeof acco[h]); so[h] = ZERO; PE(sm, PP_AS); continue; }
        sh = bitlen((uint32_t)Ws) - 12;
        if (sh < 0) sh = 0;
        k_rshr(w, u, nvalid, sh);
        PE(sm, PP_AS);
        {
            PB(k);
            k_ptab(u, nvalid, m->tmp);
            k_wsum(ly->vb + j * PX_W * PX_HD, m->tmp, nvalid, acco[h]);
            PE(k, PP_ATTK);
        }
        so[h] = sf_div(sf_pow2(sh + emax + 3), sf_int((uint32_t)P));
    }
    /* requantize the 4 heads into one int8 vector */
    PB(rq);
    Mo = ZERO;
    for (h = 0; h < PX_H; h++) {
        SF R = sf_mul(sf_int((uint32_t)k_maxabs16(acco[h], PX_HD)), so[h]);
        if (sf_less(Mo, R)) Mo = R;
    }
    if (!Mo.m) return;
    for (h = 0; h < PX_H; h++) {
        SF Fh = sf_div(sf_mul(so[h], sf_int(127)), Mo);
        if (!Fh.m) memset(yo + h * PX_HD, 0, PX_HD * 2);
        else if (Fh.e < 0 && Fh.e > -31) k_apply16c(acco[h], yo + h * PX_HD, PX_HD, Fh.m, -Fh.e, 127);
        else for (i = 0; i < PX_HD; i++) yo[h * PX_HD + i] = clamp16(sf_apply(acco[h][i], Fh), -127, 127);
    }
    bo = sf_div(Mo, sf_int(127));
    PE(rq, PP_AR);
    tern(m, ly->wo, yo, PX_D, PX_D, ao);
    F = sf_mul(sf_mul(ly->a_o, bo), sf_pow2(16));
    add_res(m->r, ao, F);
}

/* ------------------------------------------------------------------ MoE feed-forward (SPEC 5.3) */
static int16_t silu64(int16_t gi)
{
    if (gi >= 512) return gi;
    if (gi < -512) return 0;
    return px_silu[gi + 512];
}

static int ffn(PxModel *m, PxLayer *ly)
{
    static int16_t accg[PX_FF], accu[PX_FF], hq[PX_FF], yh[PX_FF];
    static int32_t hh[PX_FF];
    static int16_t y2[PX_D];
    static int32_t accd[PX_D];
    int32_t rl[PX_NE];
    int32_t Pg = 0;
    uint32_t Mh = 0;
    int i, e, top = 0, s3;
    SF b2, sr, gate, Fg, Fh, bh, Fd;
    b2 = normq(m->r, ly->g_ffn, ly->gs_ffn, 127, y2);
    PB(fr);
    for (e = 0; e < PX_NE; e++) {
        rl[e] = k_dot8(ly->router + e * PX_D, y2, PX_D);
        if (rl[e] > rl[top]) top = e;
    }
    sr = sf_mul(sf_mul(ly->s_r, b2), sf_pow2(6));
    for (e = 0; e < PX_NE; e++) {
        int32_t d = sf_apply(1, sf_mul(sf_int((uint32_t)(rl[top] - rl[e])), sr));
        if (d > 65535) d = 65535;
        Pg += px_exp12[exp_index(d)];
    }
    gate = sf_div(sf_int(4095), sf_int((uint32_t)Pg));
    PE(fr, PP_FR);
    if (m->variant == PX_W4) {          /* gate/up use W5 codes: 1024 rows share one 32-group table */
        PB(t);
        k_tab5(y2, 32, m->lut);
        k_mvw32((const int16_t *)ly->wg[top], m->lut + 16384, PX_FF, accg);
        k_mvw32((const int16_t *)ly->wu[top], m->lut + 16384, PX_FF, accu);
        PE(t, PP_TERN);
    }
#ifndef PX_NO_B5
    else {
        tern(m, ly->wg[top], y2, PX_D, PX_FF, accg);
        k_mvb5((const uint8_t *)ly->wu[top], m->lut, PX_FF, 32, accu);
    }
#endif
    Fg = sf_mul(sf_mul(ly->a_g[top], b2), sf_pow2(6));
    PB(fs);
    if (Fg.m && Fg.e < 0 && Fg.e > -31) Mh = k_ffnact(accg, accu, hh, PX_FF, Fg.m, -Fg.e, px_silu + 512);
    else
        for (i = 0; i < PX_FF; i++) {
            int16_t gi = clamp16(sf_apply(accg[i], Fg), -32768, 32767);
            int32_t v = MULS(silu64(gi), accu[i]);
            uint32_t a = v < 0 ? (uint32_t)-v : (uint32_t)v;
            hh[i] = v;
            if (a > Mh) Mh = a;
        }
    PE(fs, PP_FS);
    if (!Mh) return top;
    s3 = bitlen(Mh) - 15;
    if (s3 < 0) s3 = 0;
    {
    PB(fq);
    Fh = quant16m(hq, PX_FF, 127, yh, k_shrmax(hh, hq, PX_FF, s3));
    PE(fq, PP_FQ);
    }
    bh = sf_div(sf_mul(sf_mul(ly->a_u[top], b2), sf_pow2(s3 - 6)), Fh);
    tern_down(m, ly, top, yh, accd);
    Fd = sf_mul(sf_mul(sf_mul(ly->a_d[top], bh), gate), sf_pow2(17));
    for (i = 0; i < PX_D; i++) hq[i] = (int16_t)(accd[i] >> 1);
    add_res(m->r, hq, Fd);
    return top;
}

/* ------------------------------------------------------------------ one token (SPEC 5) */
void px_step(PxModel *m, int tok, int want_logits)
{
    int i, l, slot = m->pos & (PX_W - 1);
    int nvalid = m->pos + 1 < PX_W ? m->pos + 1 : PX_W;
    const int16_t *pc = m->head + tok * 80;
    int16_t mv = m->emb_m[tok];
    int s = m->eE + 16;
    for (i = 0; i < 80; i++) {
        int c = ((int32_t)pc[i] + 32768) / 2 - i * 256;
        int32_t a = MULS((c >> 4) - 8, mv), b = MULS((c & 15) - 8, mv);
        if (s >= 0) { m->r[2 * i] = a * ((int32_t)1 << s); m->r[2 * i + 1] = b * ((int32_t)1 << s); }
        else {
            m->r[2 * i] = (a + (1L << (-s - 1))) >> -s;
            m->r[2 * i + 1] = (b + (1L << (-s - 1))) >> -s;
        }
    }
    for (l = 0; l < PX_L; l++) {
        PB(a);
        attention(m, &m->L[l], slot, nvalid);
        PE(a, PP_ATT);
        {
            PB(f);
            m->experts[l] = ffn(m, &m->L[l]);
            PE(f, PP_FFN);
        }
    }
    m->pos++;
    if (want_logits) {
        int16_t yf[PX_D];
        PB(h);
        SF bf = normq(m->r, m->g_final, m->gs_final, 58, yf);
        k_pairs(yf, 80, m->ptabs);
        k_mvw40(m->head, m->ptabs + 16384, 2 * PX_V, m->tmp);
        k_headk(m->tmp, m->emb_m, m->K, PX_V);
        m->sigma = sf_mul(sf_pow2(m->eE + 1), bf);
        PE(h, PP_HEAD);
    }
}

void px_reset(PxModel *m)
{
    int l;
    m->pos = 0;
    memset(m->r, 0, sizeof m->r);
    for (l = 0; l < PX_L; l++) {
        PxLayer *ly = &m->L[l];
        memset(ly->sk, 0, sizeof ly->sk);
        memset(ly->sv, 0, sizeof ly->sv);
        memset(ly->kpc, 0, PX_KVH * PX_W * PX_HD * 2);
        memset(ly->vb, 0, PX_KVH * PX_W * PX_HD);
    }
}

void px_seed(PxModel *m, uint32_t seed) { m->rng = seed ? seed : 0x2545F491UL; }

static uint32_t rnd(PxModel *m)
{
    uint32_t x = m->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return m->rng = x;
}

/* ------------------------------------------------------------------ sampling (SPEC 7) */
int px_sample(PxModel *m, int temp256, int topk)
{
    int16_t id[PX_TOPK];
    int32_t kv[PX_TOPK];
    int n = 0, i, v;
    uint32_t tot = 0, pick, x, cum = 0;
    uint16_t p[PX_TOPK];
    SF sT;
    if (temp256 == 0 || topk <= 1) {
        int best = 0;
        for (v = 1; v < PX_V; v++) if (m->K[v] > m->K[best]) best = v;
        return best;
    }
    if (topk > PX_TOPK) topk = PX_TOPK;
    for (v = 0; v < PX_V; v++) {
        int32_t k = m->K[v];
        if (n == topk && k <= kv[n - 1]) continue;
        i = n < topk ? n++ : n - 1;
        while (i > 0 && kv[i - 1] < k) { kv[i] = kv[i - 1]; id[i] = id[i - 1]; i--; }
        kv[i] = k;
        id[i] = (int16_t)v;
    }
    sT = sf_div(sf_mul(m->sigma, sf_pow2(14)), sf_int((uint32_t)temp256));
    for (i = 0; i < n; i++) {
        int32_t d = sf_apply(1, sf_mul(sf_int((uint32_t)(kv[0] - kv[i])), sT));
        if (d > 65535) d = 65535;
        p[i] = (uint16_t)px_exp12[exp_index(d)];
        tot += p[i];
    }
    x = rnd(m);
    pick = ((x >> 16) * tot) >> 16;
    for (i = 0; i < n; i++) {
        cum += p[i];
        if (cum > pick) return id[i];
    }
    return id[n - 1];
}

/* ------------------------------------------------------------------ tokenizer (llm/train/tokenizer.py) */
static int merge_rank(PxModel *m, int a, int b)
{
    int lo = 0, hi = m->n_merges - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int r = m->mrank[mid];
        int ka = m->merges[2 * r], kb = m->merges[2 * r + 1];
        if (ka == a && kb == b) return r;
        if (ka < a || (ka == a && kb < b)) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static int bpe_chunk(PxModel *m, const char *c, int n, int16_t *out, int max)
{
    static int16_t sym[512];
    int i, k = 0;
    if (n > 512) n = 512;
    for (i = 0; i < n; i++) sym[i] = (int16_t)(c[i] - 32 + 4);
    while (n > 1) {
        int best = -1, ba = 0, bb = 0;
        for (i = 0; i + 1 < n; i++) {
            int r = merge_rank(m, sym[i], sym[i + 1]);
            if (r >= 0 && (best < 0 || r < best)) { best = r; ba = sym[i]; bb = sym[i + 1]; }
        }
        if (best < 0) break;
        for (i = 0, k = 0; i < n; i++) {
            if (i + 1 < n && sym[i] == ba && sym[i + 1] == bb) { sym[k++] = (int16_t)(PX_NBASE + best); i++; }
            else sym[k++] = sym[i];
        }
        n = k;
    }
    for (i = 0; i < n && i < max; i++) out[i] = sym[i];
    return i;
}

static int isalpha_(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '\''; }

static int encode_plain(PxModel *m, const char *s, int n, int16_t *out, int max)
{
    static char buf[512];
    int i = 0, k = 0;
    while (i < n && k < max) {
        int st = i, len;
        char c;
        if (s[i] == ' ' && i + 1 < n && s[i + 1] != ' ') i++;      /* space joins the next chunk */
        else if (s[i] == ' ') { i++; goto emit; }
        c = s[i];
        if (isalpha_(c)) { while (i < n && isalpha_(s[i])) i++; }
        else i++;                                                  /* digit or other: one character */
    emit:
        len = i - st;
        memcpy(buf, s + st, len);
        k += bpe_chunk(m, buf, len, out + k, max - k);
    }
    return k;
}

static const char *const SPECIALS[4] = {"<pad>", "<user>", "<pixel>", "<end>"};

int px_encode(PxModel *m, const char *s0, int n, int16_t *out, int max)
{
    char s[512];
    int i, k = 0, start = 0, j;
    if (n > 511) n = 511;
    for (i = 0; i < n; i++) s[i] = (s0[i] >= 32 && s0[i] <= 126) ? s0[i] : '?';
    for (i = 0; i < n;) {
        int hit = -1;
        if (s0[i] == '<')
            for (j = 0; j < 4; j++) {
                int L = (int)strlen(SPECIALS[j]);
                if (i + L <= n && !memcmp(s0 + i, SPECIALS[j], L)) { hit = j; break; }
            }
        if (hit < 0) { i++; continue; }
        k += encode_plain(m, s + start, i - start, out + k, max - k);
        if (k < max) out[k++] = (int16_t)hit;
        i += (int)strlen(SPECIALS[hit]);
        start = i;
    }
    k += encode_plain(m, s + start, n - start, out + k, max - k);
    return k;
}

int px_tokstr(PxModel *m, int tok, char *buf, int max)
{
    int16_t stack[64];
    int sp = 0, n = 0;
    if (tok < 4) return 0;
    stack[sp++] = (int16_t)tok;
    while (sp && n < max) {
        int t = stack[--sp];
        if (t < PX_NBASE) buf[n++] = (char)(t - 4 + 32);
        else if (sp < 62) {
            stack[sp++] = (int16_t)m->merges[2 * (t - PX_NBASE) + 1];
            stack[sp++] = (int16_t)m->merges[2 * (t - PX_NBASE)];
        }
    }
    return n;
}

/* ------------------------------------------------------------------ loading (model file v2) */
#define RBUF 32768
typedef struct {
    px_read_fn rd;
    void *ctx;
    uint8_t *buf;
    int32_t pos, len;
    int err;
    uint32_t f1, f2;           /* model checksum (adjacent: passed as an array) */
    uint32_t done, total;
    int lastpct;
    void (*progress)(int);
} Rd;

static Rd R;

#define fl(r, b) ((r)->f1 += (uint32_t)(b), (r)->f2 += (r)->f1)   /* checksum of the decoded model */

static int rfill(Rd *r)
{
    r->len = r->rd(r->ctx, r->buf, RBUF);
    r->pos = 0;
    if (r->len <= 0) { r->err = 1; r->len = 0; return 0; }
    r->done += (uint32_t)r->len;
    if (r->progress && r->total) {
        int pct = (int)(r->done / (r->total / 100 + 1));
        if (pct > 100) pct = 100;
        if (pct != r->lastpct) { r->lastpct = pct; r->progress(pct); }
    }
    return 1;
}

/* raw bytes (counted in the decoded-model checksum when ck) */
static void rbytes(Rd *r, uint8_t *d, int32_t n, int ck)
{
    while (n > 0) {
        int32_t k;
        if (r->pos >= r->len && !rfill(r)) { memset(d, 0, n); return; }
        k = r->len - r->pos;
        if (k > n) k = n;
        memcpy(d, r->buf + r->pos, k);
        if (ck) {
            uint32_t f1 = r->f1, f2 = r->f2;
            const uint8_t *p = d;
            int32_t i;
            for (i = 0; i < k; i++) { f1 += p[i]; f2 += f1; }
            r->f1 = f1;
            r->f2 = f2;
        }
        r->pos += k;
        d += k;
        n -= k;
    }
}

static int rbyte(Rd *r) { uint8_t b; rbytes(r, &b, 1, 1); return b; }
static int16_t ri16(Rd *r) { int a = rbyte(r); return (int16_t)((a << 8) | rbyte(r)); }
static uint32_t ru32(Rd *r) { uint32_t a = (uint16_t)ri16(r); return (a << 16) | (uint16_t)ri16(r); }
static uint32_t ru32n(Rd *r) { uint8_t b[4]; rbytes(r, b, 4, 0); return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]; }
static SF rsf(Rd *r) { SF s; s.m = ri16(r); s.e = ri16(r); return s; }

/* canonical Huffman decoding: 10-bit length/symbol tables for the 68000 fast path (k_huff2) */
typedef struct {
    uint16_t first[17], count[17], index[17];
    uint8_t syms[256];
    uint8_t len10[1024], sym10[1024];
} Huff;

static void huff_init(Huff *h, const uint8_t *len, int nsym)
{
    int L, s, k = 0;
    uint16_t code = 0;
    memset(h->len10, 0, sizeof h->len10);
    for (L = 1; L <= 16; L++) {
        h->count[L] = 0;
        h->first[L] = code;
        h->index[L] = (uint16_t)k;
        for (s = 0; s < nsym; s++)
            if (len[s] == L) {
                if (L <= 10) {
                    int a = code << (10 - L), b = (code + 1) << (10 - L);
                    for (; a < b; a++) { h->len10[a] = (uint8_t)L; h->sym10[a] = (uint8_t)s; }
                }
                h->syms[k++] = (uint8_t)s;
                h->count[L]++;
                code++;
            }
        code <<= 1;
    }
}

typedef struct { const uint8_t *p; uint32_t bb; int32_t bn; } HBits2;

/* n symbols from a bitstream in memory (4 bytes of zero slack after it) */
static int huff_mem(const Huff *h, HBits2 *st, uint8_t *out, int32_t n)
{
    while (n > 0) {
        long want = n > 30000 ? 30000 : n, k = k_huff2((uint32_t *)st, h->len10, h->sym10, out, want);
        int L, cd = 0;
        out += k;
        n -= k;
        if (k == want) continue;
        if (st->bn <= 16) {
            uint32_t w = ((uint32_t)st->p[0] << 8) | st->p[1];
            st->p += 2;
            st->bb |= w << (16 - st->bn);
            st->bn += 16;
        }
        for (L = 1; L <= 16; L++) {
            cd = (cd << 1) | (int)(st->bb >> 31);
            st->bb <<= 1;
            st->bn--;
            if (h->count[L] && cd >= h->first[L] && cd - h->first[L] < h->count[L]) break;
        }
        if (L > 16) return -1;
        *out++ = h->syms[h->index[L] + cd - h->first[L]];
        n--;
    }
    return 0;
}

static uint8_t trit5[243][5];
/* 4 B5 bytes (20 trits) = 5 groups of 4 trits. Per byte b, k = 0..7: 0 lo4, 1 d4, 2 3*lo3, 3 hi2,
 * 4 9*lo2, 5 hi3, 6 27*d0, 7 hi4; groups: [0]b0, [1]b0+[2]b1, [3]b1+[4]b2, [5]b2+[6]b3, [7]b3.
 * W4 codes are (idx + 81g)*2 - 32768 (group-contiguous tables). wt40: times 2 with the in-block group
 * offsets 0,162,324,486,648 folded into k = 0,1,3,5,7; wt128: times 2. */
static int16_t wt40[243 * 8], wt128[243 * 8];

static void *must(px_alloc_fn alloc, int32_t n, int *err)
{
    void *p = alloc(n);
    if (!p) *err = 2;
    return p;
}

#ifndef PX_NO_B5
/* digits (w+1, 0..2) of one row of a row-major trit stream (5 trits per byte) */
static void row_digits(const uint8_t *src, int32_t t0, int n, uint8_t *d)
{
    const uint8_t *p = src + t0 / 5;
    int j = (int)(t0 % 5), k = 0;
    while (k < n) {
        const uint8_t *t = trit5[*p++];
        for (; j < 5 && k < n; j++) d[k++] = t[j];
        j = 0;
    }
}

/* B5 runtime layout (SE) from a B5 trit stream */
static void load_b5(int rows, int n, void *dst, void *dst2, const uint8_t *src)
{
    static uint8_t d[520];
    int row, g;
    if (n == PX_D) {                       /* rows of 160 = 32 whole bytes: already the runtime layout */
        memcpy(dst, src, (size_t)rows * n / 5);
        return;
    }
    {   /* down projection: groups 0-50 (inputs 0-254) and 51-102 (255-514, zero weights 512-514) */
        uint8_t *a = (uint8_t *)dst, *bb = (uint8_t *)dst2;
        d[512] = d[513] = d[514] = 1;
        for (row = 0; row < rows; row++) {
            const uint8_t *q = d;
            row_digits(src, (int32_t)row * n, n, d);
            for (g = 0; g < 103; g++, q += 5) {
                int idx = q[0] + 3 * (q[1] + 3 * (q[2] + 3 * (q[3] + 3 * q[4])));
                if (g < 51) *a++ = (uint8_t)idx;
                else *bb++ = (uint8_t)idx;
            }
        }
    }
}
#endif

int32_t px_bytes_needed(int variant)
{
    int32_t tern = variant == PX_W4
        ? (int32_t)(PX_QKVR * 40 + PX_D * 40 + PX_NE * (2 * PX_FF * 32 + PX_D * 128)) * 2
        : (int32_t)(PX_QKVR * 32 + PX_D * 32 + PX_NE * (2 * PX_FF * 32 + PX_D * 103));
    int32_t kv = PX_KVH * PX_W * (2 * PX_HD + PX_HD);
    int32_t lut = variant == PX_W4 ? 81L * 128 * 2 : 243L * 103 * 2;
    return PX_L * (tern + kv) + PX_V * 160 + lut + 80L * 512 + 4096L * 2 + PX_V * 4L + 4000;
}

int px_load(PxModel *m, int variant, px_read_fn rd, void *ctx, px_alloc_fn alloc, px_free_fn freefn,
            void (*progress)(int))
{
    static Huff hf;
    static uint8_t lengths[256];
    Rd *r = &R;
    int err = 0, i, l, e, v, mat;
    uint32_t body_len, ck1, ck2, flags;
    uint8_t *tmpb, *tbits;
    memset(r, 0, sizeof R);
    r->rd = rd;
    r->ctx = ctx;
    r->progress = progress;
    r->lastpct = -1;
    for (i = 0; i < 243; i++) {
        int x = i, k;
        const uint8_t *t = trit5[i];
        uint8_t val[8];
        static const int16_t off[8] = {0, 162, 0, 324, 0, 486, 0, 648};
        for (l = 0; l < 5; l++) { trit5[i][l] = (uint8_t)(x % 3); x /= 3; }
        val[0] = (uint8_t)(t[0] + 3 * t[1] + 9 * t[2] + 27 * t[3]);
        val[1] = t[4];
        val[2] = (uint8_t)(3 * (t[0] + 3 * t[1] + 9 * t[2]));
        val[3] = (uint8_t)(t[3] + 3 * t[4]);
        val[4] = (uint8_t)(9 * (t[0] + 3 * t[1]));
        val[5] = (uint8_t)(t[2] + 3 * t[3] + 9 * t[4]);
        val[6] = (uint8_t)(27 * t[0]);
        val[7] = (uint8_t)(t[1] + 3 * t[2] + 9 * t[3] + 27 * t[4]);
        for (k = 0; k < 8; k++) {
            wt40[i * 8 + k] = (int16_t)(val[k] * 2 + off[k]);
            wt128[i * 8 + k] = (int16_t)(val[k] * 2);
        }
    }
    m->variant = variant;
#ifdef PX_NO_B5
    if (variant != PX_W4) return 18;
#endif
    r->buf = (uint8_t *)must(alloc, RBUF, &err);
    if (err) return err;
    if (rbyte(r) != 'P' || rbyte(r) != 'I' || rbyte(r) != 'X' || rbyte(r) != 'L') return 10;
    if (ri16(r) != 3 || ri16(r) != 64) return 11;
    {
        static const int16_t dims[9] = {160, 6, 4, 2, 40, 512, 4, 2048, 64};
        for (i = 0; i < 9; i++) if (ri16(r) != dims[i]) return 12;
    }
    m->eE = ri16(r);
    if (ri16(r) != 127 || ri16(r) != 117 || ri16(r) != 7 || ri16(r) != 58) return 13;
    body_len = ru32(r);
    ru32(r);                  /* byte sum of the file body (checked by the Python reader) */
    ru32(r);
    ru32(r);
    m->n_merges = ri16(r);
    flags = (uint16_t)ri16(r);
    ck1 = ru32(r);
    ck2 = ru32(r);
    r->total = body_len + 64;
    r->f1 = r->f2 = 0;
    tmpb = (uint8_t *)must(alloc, 16384, &err);
    tbits = (uint8_t *)must(alloc, 30720 + 8, &err);       /* one matrix's bitstream (<= 16384*15 bits) */
    m->head = (int16_t *)must(alloc, PX_V * 160L, &err);
    m->merges = (uint16_t *)must(alloc, 4L * m->n_merges + 4, &err);
    m->mrank = (uint16_t *)must(alloc, 2L * m->n_merges + 2, &err);
    m->lut = (int16_t *)must(alloc, variant == PX_W4 ? 81L * 128 * 2 : 243L * 103 * 2, &err);
    m->ptabs = (int16_t *)must(alloc, 80L * 512, &err);
    m->tmp = (int16_t *)must(alloc, 4096L * 2, &err);
    m->K = (int32_t *)must(alloc, PX_V * 4L, &err);
    if (err) return err;
    for (i = 0; i < PX_H; i++) for (l = 0; l < PX_W; l++) m->alibi[i][l] = ri16(r);
    m->gs_final = rsf(r);
    for (i = 0; i < PX_D; i++) m->g_final[i] = ri16(r);
    for (i = 0; i < PX_V; i++) m->emb_m[i] = ri16(r);
    for (i = 0; i < m->n_merges; i++) {
        int a = rbyte(r), b = rbyte(r), c = rbyte(r);
        m->merges[2 * i] = (uint16_t)((a << 4) | (b >> 4));
        m->merges[2 * i + 1] = (uint16_t)(((b & 15) << 8) | c);
        m->mrank[i] = (uint16_t)i;
    }
    if (m->n_merges & 1) rbyte(r);
    {   /* ranks sorted by (a, b) for binary search: Shell sort */
        PB(s);
        int gap;
        for (gap = m->n_merges / 2; gap > 0; gap /= 2)
            for (i = gap; i < m->n_merges; i++) {
                uint16_t x = m->mrank[i];
                uint32_t kx = ((uint32_t)m->merges[2 * x] << 16) | m->merges[2 * x + 1];
                int j = i;
                while (j >= gap) {
                    uint16_t y = m->mrank[j - gap];
                    uint32_t ky = ((uint32_t)m->merges[2 * y] << 16) | m->merges[2 * y + 1];
                    if (ky <= kx) break;
                    m->mrank[j] = y;
                    j -= gap;
                }
                m->mrank[j] = x;
            }
        PE(s, 0); px_lprof[PL_SORT] += px_prof[0]; px_prof[0] = 0;
    }
    {   /* embedding: 2048 x 80 nibble-pair bytes = head pair codes */
        PB(e);
        uint8_t *pairs = tmpb;              /* 80 symbols per row */
        if (flags & 1) {
            uint32_t nb;
            HBits2 st;
            uint8_t *eb;
            rbytes(r, lengths, 256, 0);
            nb = ru32n(r);
            huff_init(&hf, lengths, 256);
            eb = (uint8_t *)must(alloc, (int32_t)nb + 8, &err);
            if (err) return err;
            rbytes(r, eb, (int32_t)nb + (int32_t)(nb & 1), 0);
            memset(eb + nb, 0, 8 - (nb & 1));
            st.p = eb;
            st.bb = 0;
            st.bn = 0;
            for (v = 0; v < PX_V; v++) {
                int16_t *hd = m->head + v * 80;
                if (huff_mem(&hf, &st, pairs, 80)) return 14;
                k_fletcher(pairs, 80, &r->f1);
                for (i = 0; i < 80; i++) hd[i] = (int16_t)(i * 512 + pairs[i] * 2 - 32768);
            }
            if (freefn) freefn(eb);
        } else
            for (v = 0; v < PX_V; v++) {
                int16_t *hd = m->head + v * 80;
                rbytes(r, pairs, 80, 1);
                for (i = 0; i < 80; i++) hd[i] = (int16_t)(i * 512 + pairs[i] * 2 - 32768);
            }
        PE(e, 0); px_lprof[PL_EMB] += px_prof[0]; px_prof[0] = 0;
    }
    if (flags & 2) {
        rbytes(r, lengths, 244, 0);
        huff_init(&hf, lengths, 243);
    }
    for (l = 0; l < PX_L; l++) {
        PxLayer *ly = &m->L[l];
        int unit = variant == PX_W4 ? 2 : 1, g160 = variant == PX_W4 ? 40 : 32;
        ly->gs_attn = rsf(r); ly->gs_ffn = rsf(r);
        ly->a_q = rsf(r); ly->a_k = rsf(r); ly->a_v = rsf(r); ly->a_o = rsf(r); ly->s_r = rsf(r);
        for (e = 0; e < PX_NE; e++) { ly->a_g[e] = rsf(r); ly->a_u[e] = rsf(r); ly->a_d[e] = rsf(r); }
        for (i = 0; i < PX_D; i++) ly->g_attn[i] = ri16(r);
        for (i = 0; i < PX_D; i++) ly->g_ffn[i] = ri16(r);
        rbytes(r, (uint8_t *)ly->router, PX_NE * PX_D, 1);
        ly->qkv = must(alloc, (int32_t)PX_QKVR * g160 * unit, &err);
        ly->wo = must(alloc, (int32_t)PX_D * g160 * unit, &err);
        ly->kpc = (int16_t *)must(alloc, PX_KVH * PX_W * PX_HD * 2, &err);
        ly->vb = (uint8_t *)must(alloc, PX_KVH * PX_W * PX_HD, &err);
        for (e = 0; e < PX_NE; e++) {
            ly->wg[e] = must(alloc, (int32_t)PX_FF * 32 * unit, &err);     /* W5 or B5: 32 groups */
            ly->wu[e] = must(alloc, (int32_t)PX_FF * 32 * unit, &err);
            if (variant == PX_W4) { ly->wd[e] = must(alloc, PX_D * 128L * 2, &err); ly->wd2[e] = 0; }
            else { ly->wd[e] = must(alloc, PX_D * 51L, &err); ly->wd2[e] = must(alloc, PX_D * 52L, &err); }
        }
        if (err) return err;
        for (mat = 0; mat < 2 + 3 * PX_NE; mat++) {
            static const int16_t shape[3][2] = {{PX_FF, PX_D}, {PX_FF, PX_D}, {PX_D, PX_FF}};
            int rows = mat == 0 ? PX_QKVR : mat == 1 ? PX_D : shape[(mat - 2) % 3][0];
            int n = mat < 2 ? PX_D : shape[(mat - 2) % 3][1];
            int32_t nbytes = (int32_t)rows * n / 5;
            void *dst = mat == 0 ? ly->qkv : mat == 1 ? ly->wo
                      : (mat - 2) % 3 == 0 ? ly->wg[(mat - 2) / 3] : (mat - 2) % 3 == 1 ? ly->wu[(mat - 2) / 3]
                      : ly->wd[(mat - 2) / 3];
            void *dst2 = mat >= 2 && (mat - 2) % 3 == 2 ? ly->wd2[(mat - 2) / 3] : 0;
            uint32_t nb = 0;
            PB(d);
            if (flags & 2) nb = ru32n(r);
            if (nb) {                       /* Huffman-coded matrix */
                HBits2 st;
                if (nb > 30720) return 14;
                rbytes(r, tbits, (int32_t)nb + (int32_t)(nb & 1), 0);
                memset(tbits + nb, 0, 8 - (nb & 1));
                st.p = tbits;
                st.bb = 0;
                st.bn = 0;
                if (huff_mem(&hf, &st, tmpb, nbytes)) return 14;
            } else rbytes(r, tmpb, nbytes, 0);
            PE(d, 0); px_lprof[PL_TDEC] += px_prof[0]; px_prof[0] = 0;
            {
                PB(x);
                if (variant == PX_W4) {     /* W4 (gate/up: W5) codes + model checksum in one pass */
                    if (mat >= 2 && (mat - 2) % 3 != 2) k_exp5(tmpb, (int16_t *)dst, rows, &r->f1);
                    else if (n == PX_D) k_exp40(tmpb, (int16_t *)dst, rows, wt40, &r->f1);
                    else k_expg(tmpb, (int16_t *)dst, nbytes / 4, n / 4, wt128, &r->f1);
                }
#ifndef PX_NO_B5
                else {
                    k_fletcher(tmpb, nbytes, &r->f1);
                    load_b5(rows, n, dst, dst2, tmpb);
                }
#endif
                PE(x, 0); px_lprof[PL_EXPAND] += px_prof[0]; px_prof[0] = 0;
            }
        }
    }
    if (freefn) { freefn(tbits); freefn(tmpb); freefn(r->buf); }
    if (r->err) return 15;
    if (r->f1 != ck1 || r->f2 != ck2) return 17;
    (void)body_len;
    px_reset(m);
    px_seed(m, 1);
    return 0;
}

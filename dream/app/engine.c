/* Dream engine (see engine.h). Mac: layers are compiled to 68000 code (jit.c) and run by k_run
 * (run.s). Host (DM_HOST): the same arithmetic in plain C, for bit-exact tests against
 * dream/engine/picture.py without an emulator. */
#include "engine.h"

#ifdef DM_HOST
#include <stdlib.h>
void *dm_alloc(long n) { return calloc(1, (size_t)n); }
void dm_free(void *p) { free(p); }
#else
#include <MacMemory.h>
void *dm_alloc(long n) { return NewPtrClear(n); }
void dm_free(void *p) { if (p) DisposePtr((Ptr)p); }
#endif

static unsigned long rd32(const unsigned char *p) { return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3]; }
static unsigned short rd16(const unsigned char *p) { return (unsigned short)(p[0] << 8 | p[1]); }

#define TRY(x) do { if (!(x)) { m->err = 3; return 3; } } while (0)

static void *grab(DreamModel *m, long n)
{
    void *p = dm_alloc(n);
    if (p) m->ram += n;
    return p;
}

static long read_exact(dm_read_fn rd, void *ctx, void *buf, long n)
{
    return rd(ctx, buf, n) == n;
}

/* read a section header; returns payload length or -1 */
static long section(dm_read_fn rd, void *ctx, unsigned long *tag)
{
    unsigned char h[8];
    if (!read_exact(rd, ctx, h, 8)) return -1;
    *tag = rd32(h);
    return (long)rd32(h + 4);
}

static long skip_pad(dm_read_fn rd, void *ctx, long n)
{
    unsigned char z;
    return (n & 1) ? read_exact(rd, ctx, &z, 1) : 1;
}

/* read a section of big-endian int16s into native shorts (big-endian on the Mac already) */
static long read_shorts(dm_read_fn rd, void *ctx, short *dst, long n)
{
    long i;
    if (!read_exact(rd, ctx, dst, n * 2)) return 0;
#ifdef DM_HOST
    for (i = 0; i < n; i++) {
        unsigned char *b = (unsigned char *)&dst[i];
        dst[i] = (short)(b[0] << 8 | b[1]);
    }
#else
    (void)i;
#endif
    return 1;
}

static long weight_count(const LDesc *l)
{
    if (l->kind == K_PW) return (long)l->cin * l->cout;
    return l->center ? 0 : 9L * l->cout;
}

/* 5 ternary weights per byte: byte = sum (w_j + 1) 3^j */
static void unpack5(const unsigned char *src, signed char *dst, long n)
{
    static signed char T[243][5];
    static short ready;
    long i;
    if (!ready) {
        short b, j, v;
        for (b = 0; b < 243; b++)
            for (j = 0, v = b; j < 5; j++, v /= 3) T[b][j] = (signed char)(v % 3 - 1);
        ready = 1;
    }
    for (i = 0; i + 5 <= n; i += 5) {
        const signed char *t = T[*src++];
        dst[i] = t[0]; dst[i + 1] = t[1]; dst[i + 2] = t[2]; dst[i + 3] = t[3]; dst[i + 4] = t[4];
    }
    if (i < n) {
        const signed char *t = T[*src];
        short j;
        for (j = 0; i < n; j++) dst[i++] = t[j];
    }
}

/* Tensor buffers. A tensor whose border is never read (not the source of a depthwise layer) and
 * whose old values are never added to (not an addh destination) only needs its values from each
 * write to the last read before the next write; such tensors share buffers when those live
 * segments never overlap (greedy). The others (the residual streams, x_t, the output) get their
 * own zeroed buffers. */
static long alloc_tensors(DreamModel *m)
{
    short t, u, i, nslot = 0, slot[DM_MAXT];
    long size[DM_MAXT];
    short seg0[DM_MAXT][8], seg1[DM_MAXT][8], nseg[DM_MAXT];
    char own[DM_MAXT];
    for (t = 0; t < m->nt; t++) { own[t] = t < 2; nseg[t] = 0; slot[t] = -1; }
    for (i = 0; i < m->nl; i++) {
        const LDesc *l = &m->L[i];
        if (l->kind == K_DW) own[l->src] = 1;
        if (l->mode == M_ADDH) own[l->dst] = 1;
    }
    for (i = 0; i < m->nl; i++) {                  /* live segments: [write, last read] */
        const LDesc *l = &m->L[i];
        t = l->src;
        if (!own[t] && nseg[t] > 0) seg1[t][nseg[t] - 1] = i;
        t = l->dst;
        if (!own[t]) {
            if (nseg[t] >= 8) own[t] = 1;
            else { seg0[t][nseg[t]] = i; seg1[t][nseg[t]] = i; nseg[t]++; }
        }
    }
    for (t = 0; t < m->nt; t++) {
        if (own[t]) continue;
        for (u = 0; u < nslot && slot[t] < 0; u++) {
            short v, ok = 1, a, b;
            for (v = 0; v < t && ok; v++)
                if (slot[v] == u)
                    for (a = 0; a < nseg[t] && ok; a++)
                        for (b = 0; b < nseg[v] && ok; b++)
                            if (seg0[t][a] <= seg1[v][b] && seg0[v][b] <= seg1[t][a]) ok = 0;
            if (ok) slot[t] = u;
        }
        if (slot[t] < 0) { slot[t] = nslot; size[nslot++] = 0; }
        if (m->tbytes[t] > size[slot[t]]) size[slot[t]] = m->tbytes[t];
    }
    for (t = 0; t < m->nt; t++)
        if (own[t] && !(m->tens[t] = (short *)grab(m, m->tbytes[t]))) return 3;
    for (u = 0; u < nslot; u++) {
        short *p = (short *)grab(m, size[u]);
        if (!p) return 3;
        for (t = 0; t < m->nt; t++) if (slot[t] == u) m->tens[t] = p;
    }
    return 0;
}

long dm_load(DreamModel *m, dm_read_fn rd, void *ctx, void (*progress)(long, long))
{
    unsigned long tag;
    long n, i, j;
    unsigned char hb[64];
    signed char *wtmp = 0, *dig = 0;
    unsigned char *ptmp = 0;
    unsigned short *ctmp = 0;
    long maxw = 0;
#if !defined(DM_HOST)
    unsigned short *cp = 0;
    long codecap = 0;
#endif

    m->err = 0;
    m->ram = 0;
    while ((n = section(rd, ctx, &tag)) >= 0) {
        if (tag == 'HEAD') {
            if (n > 64 || !read_exact(rd, ctx, hb, n)) return m->err = 1;
            m->nt = (short)rd16(hb + 2); m->nl = (short)rd16(hb + 4); m->ntab = (short)rd16(hb + 6);
            m->nsub = (short)rd16(hb + 8); m->D = (short)rd16(hb + 10); m->N = (short)rd16(hb + 12);
            m->img = (short)rd16(hb + 14); m->patch = (short)rd16(hb + 16); m->flags = (short)rd16(hb + 18);
            m->two = (m->flags & 2) ? 1 : 0;
            m->in_shift = (m->flags & 1) ? 3 : 0;
            m->ctot = (long)rd32(hb + 20);
            if (m->nt > DM_MAXT || m->nl > DM_MAXL || m->ntab > DM_MAXTAB || m->N > DM_MAXSTEP ||
                m->nsub > DM_MAXSUBJ || rd16(hb) != 2)
                return m->err = 2;
            continue;
        }
        if (tag == 'TENS') {
            unsigned char b[6];
            for (i = 0; i < m->nt; i++) {
                if (!read_exact(rd, ctx, b, 6)) return m->err = 1;
                m->TH[i] = (short)rd16(b); m->TW[i] = (short)rd16(b + 2); m->TC[i] = (short)rd16(b + 4);
                m->tbytes[i] = (long)(m->TH[i] + 2) * (m->TW[i] + 2) * m->TC[i] * 2;
            }
            m->npix = (long)m->TH[0] * m->TW[0] * m->TC[0];
            TRY(m->x = (short *)grab(m, m->npix * 2));
            TRY(m->x0 = (short *)grab(m, m->npix * 2));
            TRY(m->z = (short *)grab(m, m->npix * 2));
            continue;
        }
        if (tag == 'TABS') {
            unsigned char b[4];
            for (i = 0; i < m->ntab; i++) {
                short *t;
                long e;
                if (!read_exact(rd, ctx, b, 4)) return m->err = 1;
                m->tlo[i] = (short)rd16(b); m->thi[i] = (short)rd16(b + 2);
#if defined(DM_HOST) || defined(DM_LUT)
                (void)t; (void)e;                 /* C requantisation clamps directly */
#else
                TRY(t = (short *)grab(m, 131072L));          /* every int16 value: saturating clamp */
                for (e = -32768; e < 32768; e++)
                    t[e + 32768] = e < m->tlo[i] ? m->tlo[i] : e > m->thi[i] ? m->thi[i] : (short)e;
                m->tabmid[i] = t + 32768;
#endif
            }
            continue;
        }
        if (tag == 'LAYR') {
            unsigned char b[20];
            long off = 0;
            for (i = 0; i < m->nl; i++) {
                LDesc *l = &m->L[i];
                if (!read_exact(rd, ctx, b, 20)) return m->err = 1;
                l->kind = b[0]; l->mode = b[1]; l->gather = b[2]; l->scatter = b[3]; l->center = b[4];
                l->tab = b[5]; l->wq = b[6]; l->pad = 0;
                l->src = (short)rd16(b + 8); l->dst = (short)rd16(b + 10); l->cin = (short)rd16(b + 12);
                l->cout = (short)rd16(b + 14); l->grid = (short)rd16(b + 16);
                m->boff[i] = off;
                off += l->cout;
                if (weight_count(l) > maxw) maxw = weight_count(l);
            }
            if (off != m->ctot) return m->err = 2;
            if (alloc_tensors(m)) return m->err = 3;
            continue;
        }
        if (tag == 'SHFT') {
            TRY(m->shift = (signed char *)grab(m, m->ctot * (m->two + 1) + 1));
            if (!read_exact(rd, ctx, m->shift, m->ctot * (m->two + 1))) return m->err = 1;
            if (!skip_pad(rd, ctx, n)) return m->err = 1;
            /* everything else is allocated before the code buffer: biases, conditioning */
            TRY(m->B0 = (short *)grab(m, m->ctot * 2));
            TRY(m->Mc = (short *)grab(m, m->ctot * 2));
            TRY(m->BT = (short *)grab(m, (long)m->N * m->ctot * 2));
            TRY(m->SB = (short *)grab(m, m->ctot * 2));
            TRY(m->B = (short *)grab(m, m->ctot * 2));
            TRY(m->E = (signed char *)grab(m, (long)m->nsub * m->D + 1));
            TRY(m->U = (signed char *)grab(m, (long)m->D * m->ctot + 1));
            continue;
        }
        if (tag == 'WGTS') {
            long done = 0, total = 0;
            for (i = 0; i < m->nl; i++) total += weight_count(&m->L[i]);
            {
                long mc = 0;                     /* largest chunk-code count (pw layers) */
                for (i = 0; i < m->nl; i++)
                    if (m->L[i].kind == K_PW) {
                        long nc = (long)((m->L[i].cout + 6) / 7) * m->L[i].cin;
                        if (nc > mc) mc = nc;
                    }
                ctmp = (unsigned short *)dm_alloc(mc * 2 + 4);
                ptmp = (unsigned char *)dm_alloc((mc + 1) / 2 * 3 + maxw / 2 + 8);
                if (!ctmp || !ptmp) return m->err = 3;
            }
#if defined(DM_HOST) || defined(DM_LUT)
            wtmp = (signed char *)dm_alloc(maxw + 9 * 512 + 8);
            dig = (signed char *)dm_alloc(2187L * 7);
            if (!wtmp || !dig) return m->err = 3;
            {
                short d[7] = {0, 0, 0, 0, 0, 0, 0}, c, jj;
                for (c = 0; c < 2187; c++) {
                    for (jj = 0; jj < 7; jj++) dig[c * 7 + jj] = (signed char)(d[jj] - 1);
                    for (jj = 0; jj < 7 && ++d[jj] == 3; jj++) d[jj] = 0;
                }
            }
#else
            {
                long md = 8;                     /* depthwise taps and 4-bit layers (ternary pw: codes) */
                for (i = 0; i < m->nl; i++) {
                    if (m->L[i].kind == K_DW && 9L * m->L[i].cout > md) md = 9L * m->L[i].cout;
                    if (m->L[i].kind == K_PW && m->L[i].wq && weight_count(&m->L[i]) > md) md = weight_count(&m->L[i]);
                }
                wtmp = (signed char *)dm_alloc(md + 8);
            }
            if (!wtmp || !jit_templates(dm_alloc)) return m->err = 3;
#endif
#ifndef DM_HOST
            jit_set_two(m->two);
#endif
#ifdef DM_LUT
            {
                long mi = 0, mo = 0;
                for (i = 0; i < m->nl; i++)
                    if (m->L[i].kind == K_PW) {
                        if (m->L[i].cin > mi) mi = m->L[i].cin;
                        if (m->L[i].cout > mo) mo = m->L[i].cout;
                    }
                if (mi > 512) return m->err = 2;
                for (i = 0; i < m->nl; i++)
                    if (m->L[i].kind == K_PW && m->L[i].wq && m->L[i].cin > mi) mi = m->L[i].cin;
                {
                    long cb = 64;                /* depthwise code (table-free requantisation) */
                    for (i = 0; i < m->nl; i++) {
                        if (m->L[i].kind == K_DW) cb += m->L[i].cout * 80L + 8;
                        if (m->L[i].kind == K_PW && m->L[i].wq)   /* 4-bit: up to 5 words per weight */
                            cb += (long)m->L[i].cin * m->L[i].cout * 10L + m->L[i].cout * 40L + 64;
                    }
                    TRY(m->codebase = cp = (unsigned short *)grab(m, cb));
                    codecap = cb;
                }
                TRY(m->lut = (short *)grab(m, (mi / 4) * 81L * 2 + 16));
                TRY(m->gin = (short *)grab(m, mi * 2));
                TRY(m->sums = (short *)grab(m, mo * 2));
            }
#elif !defined(DM_HOST)
            /* the code buffer takes the largest free block minus a reserve, shrunk afterwards */
            codecap = MaxBlock() - 48L * 1024;
            if (codecap < 32768) return m->err = 3;
            m->codebase = cp = (unsigned short *)NewPtr(codecap);
            if (!cp) return m->err = 3;
#endif
            for (i = 0; i < m->nl; i++) {
                LDesc *l = &m->L[i];
                long nw = weight_count(l), nb = (nw + 4) / 5;
                if (l->kind == K_PW && l->wq) {  /* 4-bit weights, two per byte */
                    long q;
                    nb = nw / 2;
                    if (!read_exact(rd, ctx, ptmp, nb)) return m->err = 1;
                    for (q = 0; q < nb; q++) {
                        wtmp[2 * q] = (signed char)((ptmp[q] >> 4) - 8);
                        wtmp[2 * q + 1] = (signed char)((ptmp[q] & 15) - 8);
                    }
                } else if (l->kind == K_PW) {    /* chunk-major 12-bit codes, two per 3 bytes */
                    long nc = (long)((l->cout + 6) / 7) * l->cin, q;
                    const unsigned char *b = ptmp;
                    nb = (nc + 1) / 2 * 3;
                    if (!read_exact(rd, ctx, ptmp, nb)) return m->err = 1;
                    for (q = 0; q < nc; q += 2, b += 3) {
                        ctmp[q] = (unsigned short)(b[0] << 4 | b[1] >> 4);
                        ctmp[q + 1] = (unsigned short)((b[1] & 15) << 8 | b[2]);
                    }
#if defined(DM_HOST) || defined(DM_LUT)
                    {                            /* expand to rows: w[o0 + jj][ii] */
                        long o0, ii, jj;
                        const unsigned short *cc = ctmp;
                        for (o0 = 0; o0 < l->cout; o0 += 7)
                            for (ii = 0; ii < l->cin; ii++) {
                                const signed char *dd = dig + *cc++ * 7;
                                for (jj = 0; jj < 7 && o0 + jj < l->cout; jj++)
                                    wtmp[(o0 + jj) * (long)l->cin + ii] = dd[jj];
                            }
                    }
#endif
                } else if (nw) {
                    if (!read_exact(rd, ctx, ptmp, nb)) return m->err = 1;
                    unpack5(ptmp, wtmp, nw);
                } else {
                    for (j = 0; j < l->cout; j++) wtmp[j] = 1;        /* centre tap */
                }
#if defined(DM_LUT)
                if (l->kind == K_PW && l->wq) {  /* 4-bit layer: compiled, clamps instead of tables */
                    m->code[i] = cp;
                    cp = jit_pw_ex(cp, l, wtmp, m->shift + m->boff[i] * (m->two + 1), m->TW[l->src],
                                   m->TC[l->src], m->TW[l->dst], m->TC[l->dst], l->tab != 255,
                                   l->tab == 255 ? 0 : m->tlo[l->tab], l->tab == 255 ? 0 : m->thi[l->tab]);
                    if ((char *)cp - (char *)m->codebase > codecap) return m->err = 3;
                } else if (l->kind == K_PW) {   /* W4 codes: (g*81 + sum (w_j+1) 3^j)*2 - 32768 */
                    long o, g, ng = l->cin / 4;
                    short *cd;
                    if (l->cin & 3) return m->err = 2;
                    TRY(cd = m->codes[i] = (short *)grab(m, (long)l->cout * ng * 2));
                    for (o = 0; o < l->cout; o++) {
                        const signed char *wr = wtmp + o * (long)l->cin;
                        for (g = 0; g < ng; g++, wr += 4)
                            *cd++ = (short)((g * 81 + (wr[0] + 1) + 3 * (wr[1] + 1) + 9 * (wr[2] + 1)
                                             + 27 * (wr[3] + 1)) * 2 - 32768);
                    }
                } else {                         /* depthwise: compiled, clamps instead of tables */
                    m->code[i] = cp;
                    cp = jit_dw_clamp(cp, l, wtmp, m->shift + m->boff[i] * (m->two + 1), m->TW[l->src], m->TC[l->src],
                                      m->tlo[l->tab], m->thi[l->tab]);
                    if ((char *)cp - (char *)m->codebase > codecap) return m->err = 3;
                }
#elif defined(DM_HOST)
                {
                    long cnt = nw ? nw : l->cout;
                    TRY(m->w[i] = (signed char *)grab(m, cnt));
                    for (j = 0; j < cnt; j++) m->w[i][j] = wtmp[j];
                }
#else
                m->code[i] = cp;
                if (l->kind == K_PW && l->wq)
                    cp = jit_pw(cp, l, wtmp, m->shift + m->boff[i] * (m->two + 1), m->TW[l->src], m->TC[l->src],
                                m->TW[l->dst], m->TC[l->dst]);
                else if (l->kind == K_PW)
                    cp = jit_pw_codes(cp, l, ctmp, m->shift + m->boff[i] * (m->two + 1), m->TW[l->src], m->TC[l->src],
                                      m->TW[l->dst], m->TC[l->dst]);
                else
                    cp = jit_dw(cp, l, wtmp, m->shift + m->boff[i] * (m->two + 1), m->TW[l->src], m->TC[l->src]);
                if ((char *)cp - (char *)m->codebase > codecap - 65536L) return m->err = 3;
#endif
                done += nw;
                if (progress) progress(done, total);
            }
            if (!skip_pad(rd, ctx, n)) return m->err = 1;
            dm_free(wtmp);
            dm_free(ptmp);
            dm_free(ctmp);
            dm_free(dig);
            wtmp = 0;
#if !defined(DM_HOST) && !defined(DM_LUT)
            jit_free_templates(dm_free);
#endif
#if !defined(DM_HOST) && !defined(DM_LUT)
            m->codebytes = (char *)cp - (char *)m->codebase;
            SetPtrSize((Ptr)m->codebase, m->codebytes);
            m->ram += m->codebytes;
#elif defined(DM_LUT)
            m->codebytes = (char *)cp - (char *)m->codebase;
#endif
            continue;
        }
        if (tag == 'BIAS') {
            if (!read_shorts(rd, ctx, m->B0, m->ctot)) return m->err = 1;
            continue;
        }
        if (tag == 'SUBJ') {
            long ne = (long)m->nsub * m->D, nu = (long)m->D * m->ctot;
            if (!read_exact(rd, ctx, m->E, ne) || !read_exact(rd, ctx, m->U, nu)) return m->err = 1;
            if (!read_shorts(rd, ctx, m->Mc, m->ctot)) return m->err = 1;
            if (!skip_pad(rd, ctx, n)) return m->err = 1;
            continue;
        }
        if (tag == 'TIME') {
            if (!read_shorts(rd, ctx, m->BT, (long)m->N * m->ctot)) return m->err = 1;
            continue;
        }
        if (tag == 'SAMP') {
            unsigned char b[8];
            short sz = (m->flags & 4) ? 8 : 6;            /* flags bit 2: re-noising (C3) */
            for (i = 0; i < m->N; i++) {
                if (!read_exact(rd, ctx, b, sz)) return m->err = 1;
                m->C1[i] = (short)rd16(b); m->C2[i] = (short)rd16(b + 2); m->tq[i] = rd16(b + 4);
                m->C3[i] = sz == 8 ? (short)rd16(b + 6) : 0;
            }
            continue;
        }
        if (tag == 'NAME') {
            unsigned char *p;
            TRY(m->names = (unsigned char *)grab(m, n + 1));
            if (!read_exact(rd, ctx, m->names, n)) return m->err = 1;
            if (!skip_pad(rd, ctx, n)) return m->err = 1;
            for (i = 0, p = m->names; i < m->nsub; i++) {
                m->name[i] = p;
                p += 1 + *p;
            }
            continue;
        }
        if (tag == 'END ')
            return m->err = (m->code[0] || m->w[0] || m->codes[0] || m->code[1]) ? 0 : 2;
        /* unknown section: skip */
        while (n > 0) {
            long c = n > 64 ? 64 : n;
            if (!read_exact(rd, ctx, hb, c)) return m->err = 1;
            n -= c;
        }
    }
    return m->err = 1;
}

/* ------------------------------------------------------------------ picture */
static const short RTAB[1024] = {
#include "rtab.h"
};
static const short COSQ[1024] = {
#include "costab.h"
};

static unsigned long xs32(unsigned long s)
{
    s ^= (s << 13) & 0xFFFFFFFFUL;      /* masks: unsigned long is 64-bit on the host */
    s ^= s >> 17;
    s ^= (s << 5) & 0xFFFFFFFFUL;
    return s;
}

/* n Q9 Gaussian values from the xorshift stream (FORMAT.md); *st is the stream state */
static void gen_noise(short *dst, long n, unsigned long *st)
{
    unsigned long s = *st;
    long j;
    for (j = 0; j < n; j += 2) {
        long n1, n2;
        short a, R;
        s = xs32(s);
        R = RTAB[s >> 22];
        a = (short)((s >> 12) & 1023);
        n1 = ((long)R * COSQ[a]) >> 14;
        n2 = ((long)R * COSQ[(a + 768) & 1023]) >> 14;
        n1 >>= 3; n2 >>= 3;
        dst[j] = (short)(n1 > 2047 ? 2047 : n1 < -2047 ? -2047 : n1);
        dst[j + 1] = (short)(n2 > 2047 ? 2047 : n2 < -2047 ? -2047 : n2);
    }
    *st = s;
}

void dm_begin(DreamModel *m, short subject, unsigned long seed)
{
    long c, d;
    long *acc = (long *)dm_alloc(m->ctot * 4);
    short tab[256];
    unsigned long s = seed ? seed : 1;
    m->subject = subject;
    m->seed = s;
    m->step = 0;
    /* subject biases: acc[c] = sum_d E[s][d] U[d][c] via a 256-entry product table per d */
    if (acc) {
        long ctot = m->ctot;
        const signed char *er = m->E + (long)subject * m->D;
        for (d = 0; d < m->D; d++) {
            short e = er[d], u;
            const unsigned char *ur = (const unsigned char *)m->U + d * ctot;
            long *ap = acc;
            long n = ctot;
            for (u = -128; u < 128; u++) tab[(unsigned char)u] = (short)(e * u);
            while (n-- > 0) *ap++ += tab[*ur++];
        }
        for (c = 0; c < ctot; c++)
            m->SB[c] = (short)(((long)(short)(acc[c] >> 6) * m->Mc[c]) >> 16);
        dm_free(acc);
    }
    /* noise */
    gen_noise(m->x, m->npix, &s);
    m->rng = s;
}

#if defined(DM_HOST)
/* requantisation in C, identical to the 68000 code's table lookup (FORMAT.md) */
static short sh16(short v, int k)
{
    return k >= 0 ? (short)(v >> k) : (short)(v << -k);
}

static int g_two;

static inline short rq(short acc, const signed char *kc, short b, short old, short mode, short lo, short hi)
{
    short v = sh16(acc, kc[0]), e;
    if (g_two && kc[1]) {
        int e2 = kc[1], k2 = (e2 < 0 ? -e2 : e2) - 4;
        v = (short)(e2 > 0 ? v + sh16(acc, k2) : v - sh16(acc, k2));
    }
    v = (short)(v + b);
    if (mode == M_ADDH) v = (short)(v + old);
    if (mode == M_RAW) return v;
    e = v;                                  /* the full int16 value is clamped (FORMAT.md) */
    return e < lo ? lo : e > hi ? hi : e;
}

static void run_dw_c(DreamModel *m, int i)
{
    const LDesc *l = &m->L[i];
    short Ws = m->TW[l->src], Cs = m->TC[l->src], Wd = m->TW[l->dst], Cd = m->TC[l->dst];
    const short *bias = m->B + m->boff[i];
    short lo = l->tab == 255 ? 0 : m->tlo[l->tab], hi = l->tab == 255 ? 0 : m->thi[l->tab];
    const signed char *k = m->shift + m->boff[i] * (m->two + 1), *w = m->w[i];
    short g = l->grid, y, x, o, t, cout = l->cout, tap[9];
    long srow = (long)(Ws + 2) * Cs, drow = (long)(Wd + 2) * Cd;
    const short *srcrow = m->tens[l->src] + srow + Cs;
    short *dstrow = m->tens[l->dst] + drow + Cd;
    for (t = 0; t < 9; t++) tap[t] = (short)(((t / 3 - 1) * (Ws + 2) + t % 3 - 1) * Cs);
    for (y = 0; y < g; y++, srcrow += srow, dstrow += drow) {
        const short *s0 = srcrow;
        short *d0 = dstrow;
        for (x = 0; x < g; x++, s0 += Cs, d0 += Cd) {
            const signed char *wo = w;
            for (o = 0; o < cout; o++) {
                long a = 0;
                if (l->center) {
                    a = s0[o];
                } else {
                    for (t = 0; t < 9; t++)
                        if (wo[t]) {
                            short v = s0[tap[t] + o];
                            a += wo[t] > 0 ? v : -v;
                        }
                    wo += 9;
                }
                d0[o] = rq((short)a, k + o * (g_two + 1), bias[o], 0, l->mode, lo, hi);
            }
        }
    }
}
#endif

#ifdef DM_HOST
static short *out_ptr(const LDesc *l, short *dst, short Wd, short Cd, long y, long x, long o)
{
    if (l->scatter) {
        long q = o / Cd, c = o - q * Cd;
        return dst + ((2 * y + (q >> 1) + 1) * (Wd + 2) + 2 * x + (q & 1) + 1) * Cd + c;
    }
    return dst + ((y + 1) * (Wd + 2) + x + 1) * Cd + o;
}

static void run_pw_c(DreamModel *m, int i)
{
    const LDesc *l = &m->L[i];
    short Ws = m->TW[l->src], Cs = m->TC[l->src], Wd = m->TW[l->dst], Cd = m->TC[l->dst];
    short *src = m->tens[l->src], *dst = m->tens[l->dst];
    const short *bias = m->B + m->boff[i];
    short lo = l->tab == 255 ? 0 : m->tlo[l->tab], hi = l->tab == 255 ? 0 : m->thi[l->tab];
    const signed char *k = m->shift + m->boff[i] * (m->two + 1), *w = m->w[i];
    long g = l->grid, y, x, o, ii;
    static short in[4096];
    for (y = 0; y < g; y++)
        for (x = 0; x < g; x++) {
            long n = 0;
            if (l->gather) {
                int q;
                for (q = 0; q < 4; q++) {
                    const short *p = src + ((2 * y + (q >> 1) + 1) * (Ws + 2) + 2 * x + (q & 1) + 1) * Cs;
                    for (ii = 0; ii < Cs; ii++) in[n++] = p[ii];
                }
            } else {
                const short *p = src + ((y + 1) * (Ws + 2) + x + 1) * Cs;
                for (ii = 0; ii < Cs; ii++) in[n++] = p[ii];
            }
            for (o = 0; o < l->cout; o++) {
                long a = 0;
                short *d = out_ptr(l, dst, Wd, Cd, y, x, o);
                const signed char *wr = w + o * l->cin;
                for (ii = 0; ii < l->cin; ii++) a += wr[ii] * in[ii];
                *d = rq((short)a, k + o * (g_two + 1), bias[o], *d, l->mode, lo, hi);
            }
        }
}
#endif

#ifdef DM_LUT
void k_tab4(const short *x, long groups, short *lut);
void k_mvw(const short *codes, const short *lutmid, long nout, long ngroups, short *out);
void k_rq(const short *S, const signed char *k, const short *bias, short *dst, long n, long flags,
          long lo, long hi);
void k_rq2(const short *S, const signed char *k, const short *bias, short *dst, long n, long flags,
           long lo, long hi);

static void run_pw_lut(DreamModel *m, int i)
{
    const LDesc *l = &m->L[i];
    short Ws = m->TW[l->src], Cs = m->TC[l->src], Wd = m->TW[l->dst], Cd = m->TC[l->dst];
    const short *bias = m->B + m->boff[i];
    short lo = l->tab == 255 ? 0 : m->tlo[l->tab], hi = l->tab == 255 ? 0 : m->thi[l->tab];
    const signed char *k = m->shift + m->boff[i] * (m->two + 1);
    short g = l->grid, y, x, q, ng = l->cin / 4, cout = l->cout;
    long flags = (l->mode == M_ADDH ? 1 : 0) | (l->mode == M_RAW ? 2 : 0);
    long srow = (long)(Ws + 2) * Cs, drow = (long)(Wd + 2) * Cd;
    long sstep = l->gather ? 2L * Cs : Cs, srstep = l->gather ? 2 * srow : srow;
    long dstep = l->scatter ? 2L * Cd : Cd, drstep = l->scatter ? 2 * drow : drow;
    const short *srcrow = m->tens[l->src] + srow + Cs;
    short *dstrow = m->tens[l->dst] + drow + Cd;
    long qoff[4];
    for (q = 0; q < 4; q++) qoff[q] = (q >> 1) * srow + (q & 1) * (long)Cs;
    for (y = 0; y < g; y++, srcrow += srstep, dstrow += drstep) {
        const short *sp = srcrow;
        short *dp = dstrow;
        for (x = 0; x < g; x++, sp += sstep, dp += dstep) {
            const short *in = sp;
            const short *S = m->sums;
            if (l->gather) {
                short *gp = m->gin;
                for (q = 0; q < 4; q++, gp += Cs) BlockMove((Ptr)(sp + qoff[q]), (Ptr)gp, Cs * 2L);
                in = m->gin;
            }
            k_tab4(in, ng, m->lut);
            k_mvw(m->codes[i], m->lut + 16384, cout, ng, m->sums);
            if (l->scatter) {
                for (q = 0; q < 4; q++)
                    (m->two ? k_rq2 : k_rq)(S + q * Cd, k + q * Cd * (m->two + 1), bias + q * Cd,
                                           dp + (q >> 1) * drow + (q & 1) * Cd, Cd, flags, lo, hi);
            } else {
                (m->two ? k_rq2 : k_rq)(S, k, bias, dp, cout, flags, lo, hi);
            }
        }
    }
}
#endif

#if !defined(DM_HOST)
static void run_layer_jit(DreamModel *m, int i)
{
    const LDesc *l = &m->L[i];
    short Ws = m->TW[l->src], Cs = m->TC[l->src], Wd = m->TW[l->dst], Cd = m->TC[l->dst];
    char *a0 = (char *)m->tens[l->src] + ((long)(Ws + 2) + 1) * Cs * 2;
    char *a1 = (char *)m->tens[l->dst] + ((long)(Wd + 2) + 1) * Cd * 2;
    long g = l->grid, ixs, irs, oxs, ors;
    const short *tm = l->tab == 255 ? 0 : m->tabmid[l->tab];
    if (l->kind == K_PW && l->gather) { ixs = 2L * Cs * 2; irs = (2L + Ws + 2) * Cs * 2; }
    else { ixs = (long)Cs * 2; irs = 2L * Cs * 2; }
    if (l->kind == K_PW && l->scatter) { oxs = 2L * Cd * 2; ors = (2L + Wd + 2) * Cd * 2; }
    else { oxs = (long)Cd * 2; ors = 2L * Cd * 2; }
    k_run(m->code[i], a0, a1, m->B + m->boff[i], tm, g, g, ixs, irs, oxs, ors);
}
#endif

void (*dm_layer_hook)(long done, long total);

void dm_run_layers(DreamModel *m)
{
    int i;
#ifdef DM_HOST
    g_two = m->two;
#endif
    for (i = 0; i < m->nl; i++) {
#if defined(DM_HOST)
        if (m->L[i].kind == K_PW) run_pw_c(m, i); else run_dw_c(m, i);
#elif defined(DM_LUT)
        if (m->L[i].kind == K_PW && !m->code[i]) run_pw_lut(m, i); else run_layer_jit(m, i);
#else
        run_layer_jit(m, i);
#endif
        if (dm_layer_hook) dm_layer_hook(i + 1, m->nl);
    }
}

void dm_step(DreamModel *m)
{
    long c, j;
    short i = m->step, H = m->TH[0], W = m->TW[0], C = m->TC[0], y, x, ch;
    const short *bt = m->BT + (long)i * m->ctot;
    short c1 = m->C1[i], c2 = m->C2[i];
    short *xt = m->tens[0] + (W + 3) * (long)C, *out = m->tens[1] + (W + 3) * (long)C;
    short *xs = m->x, *x0 = m->x0;
    for (c = 0; c < m->ctot; c++) m->B[c] = (short)(m->B0[c] + m->SB[c] + bt[c]);
    {
        short *p = xt, sh = m->in_shift, mx = (short)(2047 >> sh);
        for (y = 0, j = 0; y < H; y++, p += 2 * C)
            for (x = 0; x < W; x++)
                for (ch = 0; ch < C; ch++, j++) {
                    short v = (short)(xs[j] >> sh);
                    *p++ = v > mx ? mx : v < -mx ? -mx : v;
                }
    }
    dm_run_layers(m);
    {
        const short *p = out, *zp = 0;
        short c3 = m->C3[i];
        if (c3) {                          /* re-noising: fresh noise from the same stream */
            gen_noise(m->z, m->npix, &m->rng);
            zp = m->z;
        }
        for (y = 0; y < H; y++, p += 2 * C)
            for (x = 0; x < W; x++)
                for (ch = 0; ch < C; ch++) {
                    short v = *p++;
                    long nx;
                    v = v > 512 ? 512 : v < -512 ? -512 : v;
                    *x0++ = v;
                    if (zp) nx = ((long)c1 * *xs + (long)c2 * v + (long)c3 * *zp++ + 8192) >> 14;
                    else nx = ((long)c1 * *xs + (long)c2 * v + 8192) >> 14;
                    *xs++ = (short)(nx > 2047 ? 2047 : nx < -2047 ? -2047 : nx);
                }
    }
    m->step = i + 1;
}

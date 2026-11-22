/* Guess My Doodle: the classifier engine. Exactly guess/engine/ref.py:
 *   ternary layers compiled to 68000 code at load time (one call of a layer's code = one grid position:
 *   a0 = input at that position, a1 = output; d0 = input value, d1..d7 = accumulators of a chunk of
 *   up to 7 output channels; zero weights cost nothing), then per output channel the requantisation
 *       muls.w #M,dj ; add.l #B,dj ; swap dj / asr ; clamp to 0..A ; move.w dj,(a1)+
 *   A conv followed by a 2x2 max pool runs as one layer: its code is called on the 4 positions of
 *   each pooling window and keeps the largest raw sum per channel ("merge"), then a second piece of
 *   code requantises those (the requantisation never decreases, see ref.py), so only a quarter of
 *   the outputs are requantised and the pooled tensor is written directly.
 *   The first layer (3x3 conv on the binary drawing) is a 512-entry table of activation vectors,
 *   done together with the 2x2 max pool that follows it.
 * Builds for the Mac (Retro68) and, with -DGHOST, on the host (to inspect the generated code). */
#include "engine.h"

static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static long rd32(const unsigned char *p)
{
    return (long)((unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3]);
}

/* ------------------------------------------------------------------ code emitter (count, then emit) */
static unsigned short *gbuf;    /* 0 while counting */
static long gn;
#define W(x) do { if (gbuf) gbuf[gn] = (unsigned short)(x); gn++; } while (0)

#define NCODE 2187
#define ZERO7 1093
static unsigned short *tmpl;    /* NCODE x 8: count, then the add/sub words */

static int make_templates(GAlloc alloc)
{
    short d[7] = {0, 0, 0, 0, 0, 0, 0};
    int c, j;
    tmpl = (unsigned short *)alloc((long)NCODE * 16);
    if (!tmpl) return 0;
    for (c = 0; c < NCODE; c++) {
        unsigned short *t = tmpl + c * 8, n = 0;
        for (j = 0; j < 7; j++) {
            if (d[j] == 2) t[1 + n++] = (unsigned short)(0xD040 | (j + 1) << 9);         /* add.w d0,dj */
            else if (d[j] == 0) t[1 + n++] = (unsigned short)(0x9040 | (j + 1) << 9);    /* sub.w d0,dj */
        }
        t[0] = n;
        for (j = 0; j < 7 && ++d[j] == 3; j++) d[j] = 0;
    }
    return 1;
}

static void emit_shift(int n, int s, int lng)
{
    while (s > 0) {
        int k = s > 8 ? 8 : s;
        W((lng ? 0xE080 : 0xE040) | (k & 7) << 9 | n);         /* asr.l / asr.w #k,dn */
        s -= k;
    }
}

/* requantise dn (record r: M i16, S i16, B i32), clamp to 0..A unless A < 0, store with (a1)+ */
static void emit_rq(int n, const unsigned char *r, int A)
{
    short M = (short)rd16(r), S = (short)rd16(r + 2);
    long B = rd32(r + 4);
    W(0xC1FC | n << 9); W(M);                                   /* muls.w #M,dn */
    if (B) { W(0xD0BC | n << 9); W((unsigned long)B >> 16); W(B & 0xFFFF); }   /* add.l #B,dn */
    if (S >= 16) {
        W(0x4840 | n);                                          /* swap dn */
        emit_shift(n, S - 16, 0);
    } else {
        emit_shift(n, S, 1);
    }
    if (A >= 0) {
        if (S <= 16) W(0x4A40 | n);                             /* tst.w dn (asr.w set the flags) */
        W(0x6A02);                                              /* bpl.s +2 */
        W(0x7000 | n << 9);                                     /* moveq #0,dn */
        W(0x0C40 | n); W(A);                                    /* cmpi.w #A,dn */
        W(0x6F02);                                              /* ble.s +2 */
        W(0x7000 | n << 9 | (A & 0xFF));                        /* moveq #A,dn */
    }
    W(0x32C0 | n);                                              /* move.w dn,(a1)+ */
}

static short offin[4608];
static void set_steps(GModel *m);
static void run_part(GModel *m, int i, const unsigned char *bits, short r0, short r1);
static short parts(const GModel *m, int i);
static const short *trow[512];        /* the first layer's table rows (one T layer per model) */
/* blank skipping for the 3x3 stride-2 conv right after the table layer (one per model): an output
 * position whose window of the drawing (8x8 pixels, rows/columns 4Y-3..4Y+4) is empty gets the
 * output of an empty drawing, computed at load time */
static short skipL = -1;
static short *blankout;
static unsigned long colmask[8];

/* conv KxK / fully connected from chunk codes; merge = keep the max raw sum in (a1) instead of RQ */
static void emit_dense(GLayer *l, int fan, int merge)
{
    const unsigned char *rq = l->data, *pk = l->data + 8L * l->cout;
    long ci = 0, bstart = gn;
    int o0, i, j, ob = 0;
    if (l->kind == GK_L) {
        l->nblk = 1;
        l->blk[0] = gbuf ? gbuf + gn : 0;
        l->blko[0] = 0;
    }
    for (o0 = 0; o0 < l->cout; o0 += 7) {
        int m = l->cout - o0 < 7 ? l->cout - o0 : 7;
        short cur = 0;
        if (l->kind == GK_L && o0 > ob && gn - bstart > G_BLOCK_WORDS && l->nblk < G_MAXBLK) {
            W(0x43E9); W(-2 * (o0 - ob));                           /* end the block: lea, rts */
            W(0x4E75);
            bstart = gn;
            ob = o0;
            l->blk[l->nblk] = gbuf ? gbuf + gn : 0;
            l->blko[l->nblk++] = (short)o0;
        }
        for (j = 0; j < m; j++) W(0x7000 | (j + 1) << 9);         /* moveq #0,dj */
        for (i = 0; i < fan; i++, ci++) {
            const unsigned char *q = pk + (ci >> 1) * 3;
            unsigned c = ci & 1 ? (unsigned)(q[1] & 15) << 8 | q[2] : (unsigned)q[0] << 4 | q[1] >> 4;
            const unsigned short *t;
            int k;
            if (c == ZERO7) continue;
            if (offin[i] != cur) { W(0x41E8); W(offin[i] - cur); cur = offin[i]; }   /* lea d(a0),a0 */
            W(0x3018); cur += 2;                                    /* move.w (a0)+,d0 */
            t = tmpl + c * 8;
            if (gbuf) for (k = 1; k <= t[0]; k++) gbuf[gn++] = t[k];
            else gn += t[0];
        }
        if (cur) { W(0x41E8); W(-cur); }
        for (j = 0; j < m; j++) {
            if (merge) {
                W(0xB059 | (j + 1) << 9);                           /* cmp.w (a1)+,dj */
                W(0x6F04);                                          /* ble.s +4 */
                W(0x3340 | (j + 1)); W(0xFFFE);                     /* move.w dj,-2(a1) */
            } else {
                emit_rq(j + 1, rq + 8L * (o0 + j), l->A);
            }
        }
    }
    W(0x43E9); W(-2 * (l->cout - ob));                              /* lea -2cout(a1),a1 */
    W(0x4E75);                                                      /* rts */
}

/* requantise the cout merged sums at (a1) in place */
static void emit_rqpass(const GLayer *l)
{
    int c;
    for (c = 0; c < l->cout; c++) {
        W(0x3211);                                                  /* move.w (a1),d1 */
        emit_rq(1, l->data + 8L * c, l->A);
    }
    W(0x43E9); W(-2 * l->cout);
    W(0x4E75);
}

static int tern(const unsigned char *pk, long i)
{
    int c = pk[i >> 2] >> (6 - 2 * (int)(i & 3)) & 3;
    return c == 1 ? 1 : c == 3 ? -1 : 0;
}

static void emit_dw(const GLayer *l, const GTensor *s)
{
    const unsigned char *rq = l->data, *pk = l->data + 8L * l->cout;
    int C = s->C, c, t;
    short Wp = s->W + 2 * s->pad;
    for (c = 0; c < C; c++) {
        int first = 1;
        for (t = 0; t < 9; t++) {
            int w = tern(pk, (long)c * 9 + t);
            short d = (short)(((t / 3 - 1) * Wp + (t % 3 - 1)) * C * 2 + c * 2);
            if (!w) continue;
            if (first && w > 0) {
                if (d) { W(0x3228); W(d); } else W(0x3210);             /* move.w d(a0),d1 */
            } else {
                if (first) W(0x7200);                                   /* moveq #0,d1 */
                if (d) { W(w > 0 ? 0xD268 : 0x9268); W(d); }            /* add/sub.w d(a0),d1 */
                else W(w > 0 ? 0xD250 : 0x9250);
            }
            first = 0;
        }
        if (first) W(0x7200);
        emit_rq(1, rq + 8L * c, l->A);
    }
    W(0x43E9); W(-2 * C);
    W(0x4E75);
}

static void emit_layer(GLayer *l, const GTensor *s, int part)
{
    int i;
    if (l->kind == GK_D) {
        emit_dw(l, s);
    } else if (l->kind == GK_C) {
        int k = l->k, p = k / 2, fan = k * k * s->C;
        short Wp = s->W + 2 * s->pad;
        if (part == 1) { emit_rqpass(l); return; }
        for (i = 0; i < fan; i++) {
            int ky = i / (k * s->C), kx = (i / s->C) % k, ch = i % s->C;
            offin[i] = (short)(((ky - p) * Wp + (kx - p)) * s->C * 2 + ch * 2);
        }
        emit_dense(l, fan, l->fused);
    } else {
        for (i = 0; i < l->cin; i++) offin[i] = (short)(i * 2);
        emit_dense(l, l->cin, 0);
    }
}

static void emit_all(GModel *m)
{
    int i;
    for (i = 0; i < m->nlay; i++) {
        GLayer *l = &m->l[i];
        if (l->kind == GK_D || l->kind == GK_C || l->kind == GK_L) {
            long n0 = gn;
            if (gbuf) l->code = gbuf + gn;
            emit_layer(l, &m->t[l->src], 0);
            l->code1bytes = (gn - n0) * 2;
            if (l->kind == GK_C && l->fused) {
                if (gbuf) l->code2 = gbuf + gn;
                emit_layer(l, &m->t[l->src], 1);
            }
            l->codebytes = (gn - n0) * 2;
        }
    }
}

/* ------------------------------------------------------------------ load */
int g_init(GModel *m, const unsigned char *f, long len, GAlloc alloc, GFree fr)
{
    const unsigned char *p;
    long total = 0;
    int i;
    if (len < 16 || f[0] != 'G' || f[1] != 'M' || f[2] != 'D' || f[3] != 'L' || rd16(f + 4) != 2) return 1;
    m->G = (short)rd16(f + 6); m->ncls = (short)rd16(f + 8);
    m->ntens = (short)rd16(f + 10); m->nlay = (short)rd16(f + 12);
    if (m->ntens > G_MAXT || m->nlay > G_MAXL || m->G != 32) return 3;
    p = f + 16;
    for (i = 0; i < m->ntens; i++, p += 8) {
        GTensor *t = &m->t[i];
        t->H = (short)rd16(p); t->W = (short)rd16(p + 2); t->C = (short)rd16(p + 4); t->pad = (short)rd16(p + 6);
        t->base = t->org = 0;
        t->bytes = (long)(t->H + 2 * t->pad) * (t->W + 2 * t->pad) * t->C * 2;
    }
    for (i = 0; i < m->nlay; i++, p += 20) {
        GLayer *l = &m->l[i];
        l->kind = (short)rd16(p); l->src = (short)rd16(p + 2); l->dst = (short)rd16(p + 4);
        l->k = (short)rd16(p + 6); l->stride = (short)rd16(p + 8); l->cin = (short)rd16(p + 10);
        l->cout = (short)rd16(p + 12); l->A = (short)rd16(p + 14); l->len = rd32(p + 16);
        l->code = l->code2 = 0; l->codebytes = 0; l->fused = 0;
        if (l->kind == GK_C && (long)l->k * l->k * m->t[l->src].C > 4608) return 3;
        if (l->kind == GK_L && l->cin > 4608) return 3;
    }
    for (i = 0; i < m->nlay; i++) {
        m->l[i].data = p;
        p += m->l[i].len;
        if (p > f + len) return 1;
    }
    m->names = p;
    for (i = 0; i < m->nlay; i++)
        if (m->l[i].kind == GK_T) {
            int j;
            for (j = 0; j < 512; j++) trow[j] = (const short *)m->l[i].data + (long)j * m->l[i].cout;
        }
    /* T or C followed by a P on its output (C: with an even channel count): done together; the
     * unpooled tensor is never stored */
    for (i = 0; i + 1 < m->nlay; i++) {
        GLayer *l = &m->l[i];
        if ((l->kind == GK_T || (l->kind == GK_C && !(l->cout & 1))) && m->l[i + 1].kind == GK_P
            && m->l[i + 1].src == l->dst) {
            l->fused = 1;
            m->l[i + 1].fused = 1;
            m->t[l->dst].bytes = 0;
        }
    }
    for (i = 1; i < m->ntens; i++) {
        GTensor *t = &m->t[i];
        if (!t->bytes) continue;
        t->base = (short *)alloc(t->bytes);
        if (!t->base) return 2;
        t->org = t->base + ((long)t->pad * (t->W + 2 * t->pad) + t->pad) * t->C;
        total += t->bytes;
    }
    m->tensorbytes = total;
    /* compile: count, allocate, emit */
    if (!make_templates(alloc)) return 2;
    gbuf = 0; gn = 0;
    emit_all(m);
    m->codebytes = gn * 2;
    m->codebuf = (unsigned short *)alloc(m->codebytes);
    if (!m->codebuf) { fr(tmpl); tmpl = 0; return 2; }
    gbuf = m->codebuf; gn = 0;
    emit_all(m);
    gbuf = 0;
    fr(tmpl);
    tmpl = 0;
    set_steps(m);
    m->cl = m->nlay;
    /* blank skipping: T (fused with its pool, 16x16) then a 3x3 stride-2 conv (8x8, even channels) */
    skipL = -1;
    for (i = 0; i + 2 < m->nlay; i++) {
        GLayer *c = &m->l[i + 2];
        if (m->l[i].kind == GK_T && m->l[i].fused && c->kind == GK_C && !c->fused && c->k == 3 && c->stride == 2
            && c->src == m->l[i + 1].dst && m->t[c->dst].W == 8 && m->t[c->dst].H == 8 && !(c->cout & 1)) {
            static const unsigned char zero[128];
            const GTensor *d = &m->t[c->dst];
            short X, n = parts(m, i + 2);
            blankout = (short *)alloc(d->bytes);
            if (!blankout) break;
            run_part(m, i, zero, 0, 16);
            run_part(m, i + 2, 0, 0, n);                  /* bits = 0: the plain path */
            for (X = 0; X < d->bytes / 2; X++) blankout[X] = d->base[X];
            for (X = 0; X < 8; X++) {                         /* columns 4X-3 .. 4X+4, clipped to 0..31 */
                short c0 = 4 * X - 3, c1 = 4 * X + 4, k;
                unsigned long mk = 0;
                for (k = c0; k <= c1; k++) if (k >= 0 && k < 32) mk |= 0x80000000UL >> k;
                colmask[X] = mk;
            }
            skipL = (short)(i + 2);
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ run */
static void run_table(GModel *m, const GLayer *l, const unsigned char *bits, short Y0, short Y1)
{
    const GTensor *d = &m->t[m->l[(l - m->l) + 1].dst];     /* the pool's output */
    short Y, k;
    long rowst = (long)(d->W + 2 * d->pad) * l->cout;
    unsigned long rows4[4];
    for (Y = Y0; Y < Y1; Y++) {
        for (k = 0; k < 4; k++) {
            short y = 2 * Y - 1 + k;
            const unsigned char *b = bits + y * 4;
            rows4[k] = y < 0 || y >= 32 ? 0 :
                (unsigned long)b[0] << 24 | (unsigned long)b[1] << 16 | (unsigned)b[2] << 8 | b[3];
        }
        g_trow(d->org + Y * rowst, rows4, trow, l->cout);
    }
}

static void run_pool(GModel *m, const GLayer *l)
{
    const GTensor *s = &m->t[l->src], *d = &m->t[l->dst];
    int C = s->C, Y, X;
    long srow = (long)(s->W + 2 * s->pad) * C, drow = (long)(d->W + 2 * d->pad) * C;
    for (Y = 0; Y < d->H; Y++) {
        const short *a = s->org + 2 * Y * srow;
        short *o = d->org + Y * drow;
        for (X = 0; X < d->W; X++, a += 2 * C, o += C) g_max4(o, a, a + C, a + srow, a + srow + C, C);
    }
}

static void run_gmax(GModel *m, const GLayer *l)
{
    const GTensor *s = &m->t[l->src], *d = &m->t[l->dst];
    int C = s->C, y, x, c;
    long srow = (long)(s->W + 2 * s->pad) * C;
    for (c = 0; c < C; c++) d->org[c] = s->org[c];
    for (y = 0; y < s->H; y++)
        for (x = 0; x < s->W; x++) {
            const short *a = s->org + y * srow + (long)x * C;
            for (c = 0; c < C; c++) if (a[c] > d->org[c]) d->org[c] = a[c];
        }
}

/* rows [r0, r1) of layer i's output (pooled output for fused layers); L: code block r0 */
static void run_part(GModel *m, int i, const unsigned char *bits, short r0, short r1)
{
    GLayer *l = &m->l[i];
    const GTensor *s = &m->t[l->src], *d = &m->t[l->dst];
    switch (l->kind) {
    case GK_T:
        run_table(m, l, bits, r0, r1);
        break;
    case GK_P:
        if (!l->fused) run_pool(m, l);
        break;
    case GK_G:
        run_gmax(m, l);
        break;
#ifndef GHOST
    case GK_D:
    case GK_C: {
        int st = l->kind == GK_C ? l->stride : 1;
        long Wps = s->W + 2 * s->pad;
        if (l->fused) {                                      /* pooled: one call per window */
            const GTensor *pd = &m->t[m->l[i + 1].dst];
            long Wpd = pd->W + 2 * pd->pad;
            g_kpool(l->code, l->code2, s->org + 2L * r0 * st * Wps * s->C, pd->org + (long)r0 * Wpd * pd->C,
                    pd->W, r1 - r0, 2L * st * s->C * 2,
                    (2L * st * Wps - 2L * st * pd->W) * s->C * 2, (long)pd->C * 2, (Wpd - pd->W) * pd->C * 2,
                    (long)st * s->C * 2, (long)st * Wps * s->C * 2, pd->C / 2);
        } else if (i == skipL && bits) {
            long Wpd = d->W + 2 * d->pad;
            short Y, X, k;
            for (Y = r0; Y < r1; Y++) {
                unsigned long rm = 0;
                short *o = d->org + (long)Y * Wpd * d->C;
                for (k = 4 * Y - 3; k <= 4 * Y + 4; k++)
                    if (k >= 0 && k < 32) {
                        const unsigned char *b = bits + k * 4;
                        rm |= (unsigned long)b[0] << 24 | (unsigned long)b[1] << 16 | (unsigned)b[2] << 8 | b[3];
                    }
                for (X = 0; X < d->W;) {
                    if (rm & colmask[X]) {
                        short n = 1;
                        while (X + n < d->W && (rm & colmask[X + n])) n++;
                        g_krun(l->code, s->org + ((long)Y * 2 * Wps + 2L * X) * s->C, o + (long)X * d->C, n, 1,
                               4L * s->C, 0, (long)d->C * 2, 0);
                        X += n;
                    } else {
                        const long *bl = (const long *)(blankout + (d->org - d->base) + ((long)Y * Wpd + X) * d->C);
                        long *ol = (long *)(o + (long)X * d->C);
                        for (k = d->C >> 1; k > 0; k--) *ol++ = *bl++;
                        X++;
                    }
                }
            }
        } else {
            long Wpd = d->W + 2 * d->pad;
            g_krun(l->code, s->org + (long)r0 * st * Wps * s->C, d->org + (long)r0 * Wpd * d->C, d->W, r1 - r0,
                   (long)st * s->C * 2, ((long)st * Wps - (long)d->W * st) * s->C * 2, (long)d->C * 2,
                   (Wpd - d->W) * d->C * 2);
        }
        break;
    }
    case GK_L:
        g_krun(l->blk[r0], s->org, d->org + l->blko[r0], 1, 1, 0, 0, 0, 0);
        break;
#endif
    }
}

/* how many output rows (L: code blocks) a layer has, and how many to run per step */
static short parts(const GModel *m, int i)
{
    const GLayer *l = &m->l[i];
    if (l->kind == GK_L) return l->nblk;
    if (l->kind == GK_P || l->kind == GK_G) return 1;
    if (l->fused) return m->t[m->l[i + 1].dst].H;
    return m->t[l->dst].H;
}

static void set_steps(GModel *m)
{
    int i;
    for (i = 0; i < m->nlay; i++) {
        GLayer *l = &m->l[i];
        long row = 0;
        if (l->kind == GK_T) {
            row = 16L * 1500;
        } else if (l->kind == GK_D || l->kind == GK_C) {
            long W = l->fused ? m->t[m->l[i + 1].dst].W : m->t[l->dst].W;
            row = l->fused ? W * (4 * (l->code1bytes * 3 + 100) + (l->codebytes - l->code1bytes) * 3 + 200)
                           : W * (l->codebytes * 3 + 100);
        }
        l->rows = row ? (short)(G_STEP_CYCLES / row) : 1;
        if (l->rows < 1) l->rows = 1;
    }
}

void g_begin(GModel *m, const unsigned char *bits)
{
    int i;
    for (i = 0; i < 128; i++) m->bits[i] = bits[i];
    m->cl = 0;
    m->crow = 0;
}

int g_step(GModel *m)
{
    while (m->cl < m->nlay) {
        GLayer *l = &m->l[m->cl];
        short n = parts(m, m->cl), r1 = m->crow + l->rows;
        if (r1 > n) r1 = n;
        if (!(l->kind == GK_P && l->fused)) run_part(m, m->cl, m->bits, m->crow, r1);
        m->crow = r1;
        if (r1 >= n) { m->cl++; m->crow = 0; }
        if (!(l->kind == GK_P && l->fused)) return m->cl >= m->nlay;
    }
    return 1;
}

void g_run_rows(GModel *m, int i, const unsigned char *bits, short r0, short r1)
{
    run_part(m, i, bits, r0, r1);
}

void g_run_layer(GModel *m, int i, const unsigned char *bits)
{
    short n = parts(m, i), r;
    if (m->l[i].kind == GK_P && m->l[i].fused) return;
    for (r = 0; r < n; r++) run_part(m, i, bits, r, r + 1);
}

void g_run(GModel *m, const unsigned char *bits)
{
    g_begin(m, bits);
    while (!g_step(m)) { }
}

const short *g_logits(const GModel *m)
{
    return m->t[m->l[m->nlay - 1].dst].org;
}

const unsigned char *g_name(const GModel *m, int i)
{
    const unsigned char *p = m->names;
    while (i-- > 0) p += 1 + p[0];
    return p;
}

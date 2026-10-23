/* Dream kernel benchmark (M1): one denoising-network pass with random ternary weights.
 *
 * 1. allocates every tensor, the activation tables and the bias vectors;
 * 2. draws each layer's random weights with the same xorshift32 stream as
 *    dream/engine/ref.py and compiles them to 68000 code (jit.c), timing the compile;
 * 3. runs the whole network once on a random input, records a checksum of every layer's
 *    output and asks bench.lua to dump the input and output tensors (verify.py checks all of
 *    them against the Python reference);
 * 4. times the whole pass (twice) and every layer (repeated until >= 1 s) with the 60.15 Hz
 *    tick counter. The generated code has no data-dependent branches, so repeats time it fairly.
 *
 * Results go into a block tagged 'DRMB' that bench.lua finds by scanning RAM.
 */
#include <MacMemory.h>
#include <OSUtils.h>
#include "jit.h"
#include PROG_H

#define TICKS (*(volatile unsigned long *)0x16A)

typedef struct {
    unsigned long magic, notmagic, status, nentries, ndumps;
    unsigned long dumps[8][3];          /* address, size, id */
    unsigned long pad[3];
    unsigned long ent[512][4];          /* at offset 128: tag, a, b, c */
} Results;

static Results *R;
static short *tens[NT];
static long tbytes[NT];
static short *tabmid[NTAB];
static short *bias[NL];
static unsigned short *code[NL];
static unsigned long seed;

static void rec(unsigned long tag, unsigned long a, unsigned long b, unsigned long c)
{
    unsigned long n = R->nentries;
    if (n < 512) {
        R->ent[n][0] = tag; R->ent[n][1] = a; R->ent[n][2] = b; R->ent[n][3] = c;
        R->nentries = n + 1;
    }
}

static unsigned long rnd(void)
{
    unsigned long x = seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    seed = x;
    return x;
}

static void flush_dumps(void)
{
    volatile unsigned long *st = &R->status;
    *st = 2;
    while (*st == 2) { }
}

static void dump(void *p, unsigned long size, unsigned long id)
{
    unsigned long n = R->ndumps;
    R->dumps[n][0] = (unsigned long)p; R->dumps[n][1] = size; R->dumps[n][2] = id;
    R->ndumps = n + 1;
}

static unsigned long checksum(const short *p, long n)
{
    unsigned long s = 0;
    long i;
    for (i = 0; i < n; i++) s = s * 31 + (unsigned short)p[i];
    return s;
}

static void run_layer(int i)
{
    const LDesc *l = &LAY[i];
    short Hs = T_H[l->src], Ws = T_W[l->src], Cs = T_C[l->src];
    short Hd = T_H[l->dst], Wd = T_W[l->dst], Cd = T_C[l->dst];
    char *a0 = (char *)tens[l->src] + ((long)(Ws + 2) + 1) * Cs * 2;   /* interior (0,0) */
    char *a1 = (char *)tens[l->dst] + ((long)(Wd + 2) + 1) * Cd * 2;
    long g = l->grid, ixs, irs, oxs, ors;
    const short *tm = l->tab == 255 ? 0 : tabmid[l->tab];
    (void)Hs; (void)Hd;
    if (l->kind == K_PW && l->gather) {
        ixs = 2L * Cs * 2; irs = (2L + Ws + 2) * Cs * 2;
    } else {
        ixs = (long)Cs * 2; irs = 2L * Cs * 2;
    }
    if (l->kind == K_PW && l->scatter) {
        oxs = 2L * Cd * 2; ors = (2L + Wd + 2) * Cd * 2;
    } else {
        oxs = (long)Cd * 2; ors = 2L * Cd * 2;
    }
    k_run(code[i], a0, a1, bias[i], tm, g, g, ixs, irs, oxs, ors);
}

int main(void)
{
    static signed char kbuf[MAXC];
    signed char *wbuf;
    unsigned short *cp, *cbase;
    unsigned long t0, t1, magic;
    long i, j;
    volatile unsigned long m0 = 0x44524D41UL;     /* 'DRMB' - 1, so the tag isn't in the code */

    MaxApplZone();
    R = (Results *)NewPtrClear(sizeof(Results));
    if (!R) return 1;
    magic = m0 + 1;
    R->notmagic = ~magic;
    R->magic = magic;
    { Size grow; rec('MEM ', FreeMem(), MaxMem(&grow), sizeof(Results)); }

    for (i = 0; i < NT; i++) {
        tbytes[i] = (long)(T_H[i] + 2) * (T_W[i] + 2) * T_C[i] * 2;
        tens[i] = (short *)NewPtrClear(tbytes[i]);
        if (!tens[i]) { rec('FAIL', 1, i, 0); R->status = 1; return 1; }
    }
    for (i = 0; i < NTAB; i++) {
        short *t = (short *)NewPtr(131072L);
        long e;
        if (!t) { rec('FAIL', 2, i, 0); R->status = 1; return 1; }
        for (e = -32768; e < 32768; e++)
            t[e + 32768] = e < TAB_LO[i] ? TAB_LO[i] : e > TAB_HI[i] ? TAB_HI[i] : (short)e;
        tabmid[i] = t + 32768;
    }
    wbuf = (signed char *)NewPtr(MAXW);
    cbase = (unsigned short *)NewPtr(CODE_BOUND);
    if (!wbuf || !cbase) { rec('FAIL', 3, 0, 0); R->status = 1; return 1; }
    rec('ALOC', FreeMem(), CODE_BOUND, 0);

    /* weight generation alone (to separate it from the compile time), then weights -> code */
    t0 = TICKS;
    for (i = 0; i < NL; i++) {
        const LDesc *l = &LAY[i];
        long nw = (long)(l->kind == K_PW ? l->cin : (l->center ? 1 : 9)) * l->cout;
        seed = (0x2545F491UL ^ ((unsigned long)(i + 1) * 0x9E3779B9UL));
        if (l->kind == K_DW && l->center) continue;
        for (j = 0; j < nw; j++) {
            unsigned long x = rnd();
            wbuf[j] = (x >> 16) >= 21845 ? ((x >> 15) & 1 ? 1 : -1) : 0;
        }
    }
    rec('RNG ', TICKS - t0, 0, 0);
    t0 = TICKS;
    cp = cbase;
    for (i = 0; i < NL; i++) {
        const LDesc *l = &LAY[i];
        int n = l->kind == K_PW ? l->cin : (l->center ? 1 : 9);
        long nw = (long)n * l->cout;
        seed = (0x2545F491UL ^ ((unsigned long)(i + 1) * 0x9E3779B9UL));
        if (!seed) seed = 1;
        if (l->kind == K_DW && l->center) {
            for (j = 0; j < nw; j++) wbuf[j] = 1;
        } else if (l->wq) {
            static const unsigned short cum[8] = {13107, 32768, 47186, 55706, 60948, 63570, 64881, 65535};
            for (j = 0; j < nw; j++) {
                unsigned long x = rnd();
                unsigned short u = (unsigned short)(x >> 16);
                int a = 0;
                while (a < 7 && u >= cum[a]) a++;
                wbuf[j] = (signed char)((x >> 15) & 1 ? a : -a);
            }
        } else {
            for (j = 0; j < nw; j++) {
                unsigned long x = rnd();
                wbuf[j] = (x >> 16) >= 21845 ? ((x >> 15) & 1 ? 1 : -1) : 0;
            }
        }
        for (j = 0; j < l->cout; j++) kbuf[j] = (signed char)(2 + (rnd() >> 16) % 5);
        bias[i] = (short *)NewPtr(l->cout * 2L);
        if (!bias[i]) { rec('FAIL', 4, i, 0); R->status = 1; return 1; }
        for (j = 0; j < l->cout; j++) bias[i][j] = (short)(((rnd() >> 16) & 127) - 64);
        code[i] = cp;
        if (l->kind == K_PW)
            cp = jit_pw(cp, l, wbuf, kbuf, T_W[l->src], T_C[l->src], T_W[l->dst], T_C[l->dst]);
        else
            cp = jit_dw(cp, l, wbuf, kbuf, T_W[l->src], T_C[l->src]);
        if ((char *)cp > (char *)cbase + CODE_BOUND) { rec('FAIL', 5, i, 0); R->status = 1; return 1; }
    }
    t1 = TICKS;
    rec('JIT ', t1 - t0, (char *)cp - (char *)cbase, FreeMem());
    for (i = 0; i < NL; i++) {
        unsigned short *e = i + 1 < NL ? code[i + 1] : cp;
        rec('CODE', i, (char *)e - (char *)code[i], checksum((short *)code[i], e - code[i]));
    }

    /* random input */
    seed = 0x01234567UL;
    {
        short H = T_H[T_XT], Wd = T_W[T_XT], C = T_C[T_XT];
        short y, x, c;
        for (y = 0; y < H; y++)
            for (x = 0; x < Wd; x++)
                for (c = 0; c < C; c++)
                    tens[T_XT][((long)(y + 1) * (Wd + 2) + x + 1) * C + c] = (short)((rnd() >> 16) % 4095 - 2047);
    }
    dump(tens[T_XT], tbytes[T_XT], 1);
    flush_dumps();

    /* one pass, with checksums */
    for (i = 0; i < NL; i++) {
        const LDesc *l = &LAY[i];
        run_layer(i);
        rec('SUM ', i, checksum(tens[l->dst], tbytes[l->dst] / 2), 0);
    }
    dump(tens[T_OUT], tbytes[T_OUT], 2);
    flush_dumps();

    /* whole pass, twice */
    for (j = 0; j < 2; j++) {
        t0 = TICKS;
        for (i = 0; i < NL; i++) run_layer(i);
        rec('STEP', TICKS - t0, 1, j);
    }
    /* each layer, repeated until at least 60 ticks */
    for (i = 0; i < NL; i++) {
        long n = 0;
        t0 = TICKS;
        do { run_layer(i); n++; } while (TICKS - t0 < 60);
        rec('LAYR', i, TICKS - t0, n);
    }
    rec('DONE', 0, 0, 0);
    R->status = 1;
    for (;;) { }
    return 0;
}

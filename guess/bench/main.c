/* Guess My Doodle engine benchmark (M0).
 *
 * Loads "Guess Model" (a model file) and "Guess Tests" (u16 count, then count 32x32 bitmaps of 128
 * bytes) from the System Folder, compiles the model (timed), runs every test drawing and records
 * a checksum of its logits, records a checksum of every tensor for test 0, times whole passes and
 * every layer (repeated until >= 1 s). Results go into a block tagged 'GSBM' for bench.lua.
 */
#include <Files.h>
#include <MacMemory.h>
#include <OSUtils.h>
#include "engine.h"

#define TICKS (*(volatile unsigned long *)0x16A)

typedef struct {
    unsigned long magic, notmagic, status, nentries, ndumps;
    unsigned long dumps[8][3];
    unsigned long pad[3];
    unsigned long ent[1024][4];          /* at offset 128: tag, a, b, c */
} Results;

static Results *R;
static GModel M;

static void rec(unsigned long tag, unsigned long a, unsigned long b, unsigned long c)
{
    unsigned long n = R->nentries;
    if (n < 1024) {
        R->ent[n][0] = tag; R->ent[n][1] = a; R->ent[n][2] = b; R->ent[n][3] = c;
        R->nentries = n + 1;
    }
}

static void *zalloc(long n) { return NewPtrClear(n); }
static void zfree(void *p) { DisposePtr((Ptr)p); }

static unsigned long csum_tensor(const GTensor *t)
{
    unsigned long s = 0;
    int y, x, c;
    long row = (long)(t->W + 2 * t->pad) * t->C;
    for (y = 0; y < t->H; y++)
        for (x = 0; x < t->W; x++) {
            const short *p = t->org + y * row + (long)x * t->C;
            for (c = 0; c < t->C; c++) s = s * 31 + (unsigned short)p[c];
        }
    return s;
}

static Ptr read_file(const unsigned char *name, long *len)
{
    short ref;
    long n;
    Ptr p;
    if (FSOpen(name, 0, &ref) != noErr) return 0;
    GetEOF(ref, &n);
    p = NewPtr(n);
    if (p && FSRead(ref, &n, p) != noErr) p = 0;
    FSClose(ref);
    *len = n;
    return p;
}

int main(void)
{
    static const unsigned char mname[] = "\pGuess Model", tname[] = "\pGuess Tests";
    unsigned long t0, magic;
    volatile unsigned long m0 = 0x4753424CUL;        /* 'GSBM' - 1, so the tag isn't in the code */
    long mlen, tlen, i, j, n;
    Ptr mf, tf;
    int e;

    MaxApplZone();
    R = (Results *)NewPtrClear(sizeof(Results));
    if (!R) return 1;
    magic = m0 + 1;
    R->notmagic = ~magic;
    R->magic = magic;
    rec('MEM ', FreeMem(), 0, 0);
    /* the System Folder is the default volume's blessed folder; the app is started from there too */
    t0 = TICKS;
    mf = read_file(mname, &mlen);
    tf = read_file(tname, &tlen);
    rec('READ', TICKS - t0, mlen, tlen);
    if (!mf || !tf) { rec('FAIL', 1, (unsigned long)mf, (unsigned long)tf); R->status = 1; for (;;) { } }
    t0 = TICKS;
    e = g_init(&M, (const unsigned char *)mf, mlen, zalloc, zfree);
    rec('JIT ', TICKS - t0, M.codebytes, M.tensorbytes);
    rec('FREE', FreeMem(), e, 0);
    if (e) { R->status = 1; for (;;) { } }
    for (i = 0; i < M.nlay; i++) rec('LCOD', i, M.l[i].kind, M.l[i].codebytes);
    n = ((unsigned char *)tf)[0] << 8 | ((unsigned char *)tf)[1];
    /* test 0: every tensor */
    {
        const unsigned char *b = (const unsigned char *)tf + 2;
        for (i = 0; i < M.nlay; i++) {
            g_run_layer(&M, i, b);
            if (M.t[M.l[i].dst].org) rec('TSUM', i, csum_tensor(&M.t[M.l[i].dst]), 0);
        }
    }
    /* all tests: logits checksum, arg max, time per pass */
    for (j = 0; j < n; j++) {
        const unsigned char *b = (const unsigned char *)tf + 2 + j * 128;
        const short *lo;
        unsigned long s = 0;
        int k, best = 0;
        t0 = TICKS;
        g_run(&M, b);
        t0 = TICKS - t0;
        lo = g_logits(&M);
        for (k = 0; k < M.ncls; k++) {
            s = s * 31 + (unsigned short)lo[k];
            if (lo[k] > lo[best]) best = k;
        }
        rec('TEST', j, s, (unsigned long)best << 16 | t0);
    }
    /* whole pass on test 0, 10 times */
    t0 = TICKS;
    for (j = 0; j < 10; j++) g_run(&M, (const unsigned char *)tf + 2);
    rec('PASS', TICKS - t0, 10, 0);
    /* each layer on test 0, repeated until >= 60 ticks */
    g_run(&M, (const unsigned char *)tf + 2);
    for (i = 0; i < M.nlay; i++) {
        long k = 0;
        t0 = TICKS;
        do { g_run_layer(&M, i, (const unsigned char *)tf + 2); k++; } while (TICKS - t0 < 60);
        rec('LAYR', i, TICKS - t0, k);
    }
    /* the first layer in one call (as g_step runs it), 20 times */
    t0 = TICKS;
    for (j = 0; j < 20; j++) g_run_rows(&M, 0, (const unsigned char *)tf + 2, 0, 16);
    rec('TTAB', TICKS - t0, 20, 0);
    rec('DONE', 0, 0, 0);
    R->status = 1;
    for (;;) { }
    return 0;
}

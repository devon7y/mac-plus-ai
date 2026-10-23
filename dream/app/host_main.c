/* Host test driver: dream_host <model.drm> <subject> <seed> <out.bin>
 * Writes, for each step, the x0 estimate (npix big-endian int16, storage order), then the
 * subject biases SB (ctot int16). */
#include <stdio.h>
#include <stdlib.h>
#include "engine.h"

static long rdf(void *ctx, void *buf, long n) { return (long)fread(buf, 1, (size_t)n, (FILE *)ctx); }

static void put16(FILE *f, short v) { fputc((v >> 8) & 255, f); fputc(v & 255, f); }

int main(int argc, char **argv)
{
    static DreamModel m;
    FILE *f, *o;
    long i, s;
    if (argc < 5) { fprintf(stderr, "usage: %s model subject seed out\n", argv[0]); return 2; }
    f = fopen(argv[1], "rb");
    if (!f || dm_load(&m, rdf, f, NULL)) { fprintf(stderr, "load failed: %ld\n", m.err); return 1; }
    fclose(f);
    o = fopen(argv[4], "wb");
    dm_begin(&m, (short)atoi(argv[2]), strtoul(argv[3], 0, 10));
    for (s = 0; s < m.N; s++) {
        dm_step(&m);
        for (i = 0; i < m.npix; i++) put16(o, m.x0[i]);
    }
    for (i = 0; i < m.ctot; i++) put16(o, m.SB[i]);
    fclose(o);
    printf("ok: %d layers, %d steps, ctot %ld, ram %ld\n", m.nl, m.N, m.ctot, m.ram);
    return 0;
}

/* Host test driver for the engine (differential tests against llm/ref).
 *   pxhost <model> <W4|B5> logits <ids...>        teacher-forced: per token, experts and K checksum
 *   pxhost <model> <W4|B5> gen <seed> <temp256> <topk> <n> <ids...>
 *   pxhost <model> <W4|B5> enc                     one line of stdin per output line of ids
 *   pxhost <model> <W4|B5> dec <ids...>            decoded text */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"

static int32_t frd(void *ctx, void *buf, int32_t n) { return (int32_t)fread(buf, 1, n, (FILE *)ctx); }
static void *al(int32_t n) { return calloc(1, n); }

int main(int argc, char **argv)
{
    static PxModel m;
    FILE *f;
    int variant, err, i;
    if (argc < 4) return 2;
    f = fopen(argv[1], "rb");
    if (!f) return 3;
    variant = !strcmp(argv[2], "B5") ? PX_B5 : PX_W4;
    err = px_load(&m, variant, frd, f, al, free, 0);
    fclose(f);
    if (err) { printf("load error %d\n", err); return 4; }
    if (!strcmp(argv[3], "logits")) {
        for (i = 4; i < argc; i++) {
            uint32_t h = 2166136261u;
            int v, l;
            px_step(&m, atoi(argv[i]), 1);
            for (v = 0; v < PX_V; v++) { h ^= (uint32_t)m.K[v]; h *= 16777619u; }
            printf("tok %s experts", argv[i]);
            for (l = 0; l < PX_L; l++) printf(" %d", m.experts[l]);
            printf(" sigma %d %d hash %08x K0 %ld\n", m.sigma.m, m.sigma.e, h, (long)m.K[0]);
        }
    } else if (!strcmp(argv[3], "gen")) {
        int n = atoi(argv[7]), np = argc - 8;
        px_seed(&m, (uint32_t)strtoul(argv[4], 0, 10));
        for (i = 0; i < np; i++) px_step(&m, atoi(argv[8 + i]), i == np - 1);
        for (i = 0; i < n; i++) {
            int t = px_sample(&m, atoi(argv[5]), atoi(argv[6]));
            printf("%d ", t);
            if (i < n - 1) px_step(&m, t, 1);
        }
        printf("\n");
    } else if (!strcmp(argv[3], "enc")) {
        char line[600];
        int16_t ids[600];
        while (fgets(line, sizeof line, stdin)) {
            int n = (int)strlen(line), k;
            if (n && line[n - 1] == '\n') line[--n] = 0;
            k = px_encode(&m, line, n, ids, 600);
            for (i = 0; i < k; i++) printf("%d ", ids[i]);
            printf("\n");
        }
    } else if (!strcmp(argv[3], "dec")) {
        char buf[64];
        for (i = 4; i < argc; i++) { int n = px_tokstr(&m, atoi(argv[i]), buf, 64); fwrite(buf, 1, n, stdout); }
        printf("\n");
    }
    return 0;
}

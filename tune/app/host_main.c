/* Host test driver: compose tunes with the C engine and print their tokens.
   host_tune model.bin type meter mode seed [invT cut maxbars] */
#include <stdio.h>
#include <stdlib.h>
#include "engine.h"


int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    long len;
    void *blob, *work;
    TuneModel m;
    TuneGen g;
    short t;
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    blob = malloc(len); fread(blob, 1, len, f); fclose(f);
    tm_swap(blob, len);
    if (tm_load(&m, blob, len)) { printf("load failed\n"); return 1; }
    work = malloc(tg_bytes(&m));
    tg_init(&g, &m, work);
    if (argc > 6) { g.invT = atoi(argv[6]); g.cut = atoi(argv[7]); g.maxbars = atoi(argv[8]); }
    tg_start(&g, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), (uint32_t)strtoul(argv[5], 0, 10));
    printf("1");
    for (;;) {
        t = tg_sample(&g);
        printf(" %d", t);
        if (t == TK_EOS) break;
        tg_feed(&g, t);
    }
    printf("\n");
    return 0;
}

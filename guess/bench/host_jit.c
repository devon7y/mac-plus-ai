/* Host build of the engine's compiler: load a model file, compile it, write each layer's
 * generated 68000 code (big-endian) to OUT_<i>.bin and print the sizes.
 *   cc -DGHOST -I../app host_jit.c ../app/engine.c -o host_jit && ./host_jit model.gmdl OUTPREFIX */
#include <stdio.h>
#include <stdlib.h>
#include "engine.h"
static void *zalloc(long n) { return calloc(1, n); }
static void zfree(void *p) { free(p); }
int main(int argc, char **argv)
{
    static GModel m;
    FILE *f = fopen(argv[1], "rb");
    static unsigned char buf[2000000];
    long len = (long)fread(buf, 1, sizeof buf, f), total = 0;
    int i, r = g_init(&m, buf, len, zalloc, zfree);
    if (r) { printf("g_init error %d\n", r); return 1; }
    for (i = 0; i < m.nlay; i++) {
        GLayer *l = &m.l[i];
        printf("layer %d kind %d cin %d cout %d code %ld\n", i, l->kind, l->cin, l->cout, l->codebytes);
        if (l->code && argc > 2) {
            char nm[512];
            long j;
            FILE *o;
            snprintf(nm, sizeof nm, "%s_%d.bin", argv[2], i);
            o = fopen(nm, "wb");
            for (j = 0; j < l->codebytes / 2; j++) { fputc(l->code[j] >> 8, o); fputc(l->code[j] & 255, o); }
            fclose(o);
        }
        total += l->codebytes;
    }
    printf("code total %ld bytes, tensors %ld bytes\n", total, m.tensorbytes);
    return 0;
}
void g_max4(short *o, const short *r0, const short *r1, const short *r2, const short *r3, long C)
{
    long c;
    for (c = 0; c < C; c++) {
        short a = r0[c];
        if (r1[c] > a) a = r1[c];
        if (r2[c] > a) a = r2[c];
        if (r3[c] > a) a = r3[c];
        o[c] = a;
    }
}
void g_trow(short *o, const unsigned long *rows4, const short *const *trow, long C) { (void)o; (void)rows4; (void)trow; (void)C; }

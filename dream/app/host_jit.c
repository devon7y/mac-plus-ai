/* Host check of the Mac compiler: jit_pw_codes (v2 chunk codes) and jit_dw for every layer of a
 * model file; writes per layer: u32 word count, then the words (big-endian). test_jit.py compares
 * them with dream/engine/jit.py. Usage: host_jit <model.drm> <out.bin> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jit.h"

static void *al(long n) { return calloc(1, (size_t)n); }
static unsigned long rd32(const unsigned char *p) { return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3]; }
static unsigned short rd16(const unsigned char *p) { return (unsigned short)(p[0] << 8 | p[1]); }

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"), *o = fopen(argv[2], "wb");
    static unsigned char buf[4 << 20];
    long n = (long)fread(buf, 1, sizeof buf, f), pos = 0, i;
    unsigned char *S[32] = {0};
    char tags[32][5];
    int ns = 0, nt = 0, nl = 0;
    short TW[32], TC[32], tlo[8], thi[8];
    int clamp = argc > 3, two = 0;
    LDesc L[96];
    static unsigned short code[1 << 20], ctmp[1 << 18];
    signed char *shift = 0, wd[9 * 1024];
    const unsigned char *wg;
    while (pos < n) {
        long len = (long)rd32(buf + pos + 4);
        memcpy(tags[ns], buf + pos, 4); tags[ns][4] = 0;
        S[ns++] = buf + pos + 8;
        pos += 8 + len + (len & 1);
        if (!strcmp(tags[ns - 1], "END ")) break;
    }
    for (i = 0; i < ns; i++) {
        unsigned char *p = S[i];
        if (!strcmp(tags[i], "HEAD")) { nt = rd16(p + 2); nl = rd16(p + 4); two = (rd16(p + 18) & 2) != 0; }
        if (!strcmp(tags[i], "TENS")) { int t; for (t = 0; t < nt; t++) { TW[t] = rd16(p + 6 * t + 2); TC[t] = rd16(p + 6 * t + 4); } }
        if (!strcmp(tags[i], "LAYR")) { int l; for (l = 0; l < nl; l++) { unsigned char *b = p + 20 * l;
            L[l].kind = b[0]; L[l].mode = b[1]; L[l].gather = b[2]; L[l].scatter = b[3]; L[l].center = b[4];
            L[l].tab = b[5]; L[l].wq = b[6]; L[l].src = rd16(b + 8); L[l].dst = rd16(b + 10); L[l].cin = rd16(b + 12);
            L[l].cout = rd16(b + 14); L[l].grid = rd16(b + 16); } }
        if (!strcmp(tags[i], "SHFT")) shift = (signed char *)p;
        if (!strcmp(tags[i], "TABS")) { int t; for (t = 0; t < 8; t++) { tlo[t] = (short)rd16(p + 4 * t); thi[t] = (short)rd16(p + 4 * t + 2); } }
        if (!strcmp(tags[i], "WGTS")) wg = p;
    }
    jit_templates(al);
    jit_set_two(two);
    {
        long off = 0;
        int l;
        for (l = 0; l < nl; l++) {
            LDesc *d = &L[l];
            unsigned short *e;
            if (d->kind == K_PW && d->wq) {
                long nw = (long)d->cin * d->cout, q;
                static signed char w4[1 << 16];
                for (q = 0; q < nw / 2; q++, wg++) { w4[2 * q] = (signed char)((*wg >> 4) - 8); w4[2 * q + 1] = (signed char)((*wg & 15) - 8); }
                e = jit_pw_ex(code, d, w4, shift + off * (two + 1), TW[d->src], TC[d->src], TW[d->dst], TC[d->dst],
                              clamp && d->tab != 255, d->tab == 255 ? 0 : tlo[d->tab], d->tab == 255 ? 0 : thi[d->tab]);
            } else if (d->kind == K_PW) {
                long nc = (long)((d->cout + 6) / 7) * d->cin, q;
                for (q = 0; q < nc; q += 2, wg += 3) {
                    ctmp[q] = (unsigned short)(wg[0] << 4 | wg[1] >> 4);
                    ctmp[q + 1] = (unsigned short)((wg[1] & 15) << 8 | wg[2]);
                }
                e = jit_pw_codes(code, d, ctmp, shift + off * (two + 1), TW[d->src], TC[d->src], TW[d->dst], TC[d->dst]);
            } else {
                long nw = d->center ? 0 : 9L * d->cout, j, b;
                if (nw) {
                    for (j = 0, b = 0; j < nw; b++) { int v = wg[b], t; for (t = 0; t < 5 && j < nw; t++, v /= 3) wd[j++] = (signed char)(v % 3 - 1); }
                    wg += (nw + 4) / 5;
                } else for (j = 0; j < d->cout; j++) wd[j] = 1;
                e = clamp ? jit_dw_clamp(code, d, wd, shift + off * (two + 1), TW[d->src], TC[d->src], tlo[d->tab], thi[d->tab])
                          : jit_dw(code, d, wd, shift + off * (two + 1), TW[d->src], TC[d->src]);
            }
            {
                unsigned long cnt = (unsigned long)(e - code);
                unsigned char h[4] = {cnt >> 24, cnt >> 16, cnt >> 8, cnt};
                fwrite(h, 1, 4, o);
            }
            {
                long k;
                for (k = 0; k < e - code; k++) { fputc(code[k] >> 8, o); fputc(code[k] & 255, o); }
            }
            off += d->cout;
        }
    }
    fclose(o);
    return 0;
}

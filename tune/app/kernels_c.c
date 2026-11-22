/* Host (C) versions of the 68000 kernels in kernels68k.s: same results (exact dot products). */
#include "engine.h"

void k_tab5(const short *x, long groups, short *lut)
{
    long g;
    short i, j, t, d;
    for (g = 0; g < groups; g++) {
        for (i = 0; i < 243; i++) {
            long v = 0;
            t = i;
            for (j = 0; j < 5; j++) {
                d = t % 3;
                t /= 3;
                v += (long)(d - 1) * x[5 * g + j];
            }
            lut[243 * g + i] = (short)v;
        }
    }
}

void k_mv(const short *codes, const short *lutmid, long nrows, short *out, long G)
{
    /* lutmid = lut + 32768 bytes; code = byte offset - 32768 */
    long r;
    short g;
    for (r = 0; r < nrows; r++) {
        short acc = 0;
        for (g = 0; g < G; g++) {
            short c = *codes++;
            acc = (short)(acc + *(const short *)((const char *)lutmid + c));
        }
        out[r] = acc;
    }
}

static short sat16(long v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (short)v; }
static short gh16(short acc, short M, long k) { return (short)((((long)acc * M) >> 16) << k); }

void k_gru(const short *x, const short *acc, const short *Mb, long k, short *h, long H,
           const short *sigmid, const short *tanhmid)
{
    long i;
    for (i = 0; i < H; i++) {
        short p, r, z, n, v;
        p = sat16((long)x[0] + gh16(acc[0], Mb[0], k));
        r = sigmid[p >> 3];
        p = sat16((long)x[1] + gh16(acc[1], Mb[1], k));
        z = sigmid[p >> 3];
        v = sat16((long)gh16(acc[2], Mb[2], k) + Mb[3]);
        p = sat16((long)x[2] + (((long)r * v) >> 15));
        n = tanhmid[p >> 3];
        h[i] = (short)(n + (((long)z * (short)(h[i] - n)) >> 15));
        x += 3; acc += 3; Mb += 4;
    }
}

void k_scale(const short *acc, const short *M, long k, const short *b, short *out, long n)
{
    long i;
    for (i = 0; i < n; i++)
        out[i] = sat16((long)gh16(acc[i], M[i], k) + b[i]);
}

void k_sum5(short *out, long n, const short *p0, const short *p1, const short *p2,
            const short *p3, const short *p4, long accumulate)
{
    long i;
    for (i = 0; i < n; i++) {
        short v = (short)(p0[i] + p1[i] + p2[i] + p3[i] + p4[i]);
        out[i] = accumulate ? (short)(out[i] + v) : v;
    }
}

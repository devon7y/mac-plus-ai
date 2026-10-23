/* Blockworld: the save file's LZ77 codec (see lz.h). The packer is greedy with one remembered
 * position per hash of 3 bytes (fast on the 68000: no multiplies); tools/bwsave.py has the same
 * packer (byte for byte) and a slower, tighter one for the shipped default world. */
#include <string.h>
#include "lz.h"

#define HMASK ((1 << LZ_HBITS) - 1)
#define HASH(p) ((unsigned short)((((unsigned short)(p)[0] << 6) ^ ((unsigned short)(p)[1] << 3) ^ (p)[2]) & HMASK))

static unsigned char *put_len(unsigned char *o, long r)
{
    while (r >= 255) { *o++ = 255; r -= 255; }
    *o++ = (unsigned char)r;
    return o;
}

/* a token: nlit literals from lit, then (mlen > 0) a match of mlen bytes off bytes back */
static unsigned char *emit(unsigned char *o, const unsigned char *lit, long nlit, long mlen, long off)
{
    unsigned char *tok = o++;
    long m = mlen ? mlen - LZ_MINM : 0;
    *tok = (unsigned char)(((nlit >= 15 ? 15 : nlit) << 4) | (m >= 15 ? 15 : m));
    if (nlit >= 15) o = put_len(o, nlit - 15);
    memcpy(o, lit, nlit);
    o += nlit;
    if (mlen) {
        *o++ = (unsigned char)(off >> 8);
        *o++ = (unsigned char)off;
        if (m >= 15) o = put_len(o, m - 15);
    }
    return o;
}

long lz_pack_c(const unsigned char *in, long n, unsigned char *out, void *work)
{
    const unsigned char **ht = (const unsigned char **)work, *q = in, *anchor = in, *end = in + n;
    const unsigned char *last = end - LZ_MINM;           /* the last position with 3 bytes to hash */
    unsigned char *o = out;
    short k;
    for (k = 0; k <= HMASK; k++) ht[k] = 0;
    while (q <= last) {
        unsigned short h = HASH(q);
        const unsigned char *c = ht[h];
        ht[h] = q;
        if (c && q - c <= 65535 && c[0] == q[0] && c[1] == q[1] && c[2] == q[2]) {
            const unsigned char *p = c + LZ_MINM, *s = q + LZ_MINM;
            while (s < end && *p == *s) { p++; s++; }
            o = emit(o, anchor, q - anchor, s - q, q - c);
            q = anchor = s;
        } else
            q++;
    }
    if (anchor < end) o = emit(o, anchor, end - anchor, 0, 0);
    return o - out;
}

short lz_unpack_c(const unsigned char *in, long len, unsigned char *out, long n)
{
    const unsigned char *ie = in + len;
    unsigned char *o = out, *oe = out + n;
    while (o < oe) {
        unsigned short t, off;
        long l;
        const unsigned char *m;
        if (in >= ie) return -1;
        t = *in++;
        l = t >> 4;
        if (l == 15) {
            unsigned char b;
            do { if (in >= ie) return -1; b = *in++; l += b; } while (b == 255);
        }
        if (l > oe - o || l > ie - in) return -1;
        memcpy(o, in, l);
        o += l; in += l;
        if (o >= oe) break;
        if (ie - in < 2) return -1;
        off = (unsigned short)((in[0] << 8) | in[1]);
        in += 2;
        l = t & 15;
        if (l == 15) {
            unsigned char b;
            do { if (in >= ie) return -1; b = *in++; l += b; } while (b == 255);
        }
        l += LZ_MINM;
        if (!off || off > o - out || l > oe - o) return -1;
        m = o - off;
        while (l--) *o++ = *m++;
    }
    return 0;
}

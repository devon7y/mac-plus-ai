/* Guess My Doodle: the drawing -> the classifier's 32x32 input. An exact port of
 * guess/engine/raster.py (the spec); test_raster.py checks it against the Python on the host. */
#include "raster.h"

#define G 32
#define MARGIN 1
#define E (G - 1 - 2 * MARGIN)
#define MINSIDE 48

/* round(a / b) = (2a + b) / 2b for a >= 0, b > 0; (2a + b) < 65536 here, so one divu.w */
static unsigned short rdiv(unsigned short a, unsigned short b)
{
#ifdef GHOST
    return (unsigned short)((2UL * a + b) / (2UL * b));
#else
    unsigned long n = 2UL * a + b;
    __asm__("divu.w %1,%0" : "+d"(n) : "d"((unsigned short)(2 * b)));
    return (unsigned short)n;
#endif
}

static void pix(unsigned char *bits, short x, short y)
{
    bits[y * 4 + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
}

/* p_i = a + sign(d) * ((2 i |d| + n) div 2n), i = 0..n, with the quotient kept incrementally */
static void line(unsigned char *bits, short ax, short ay, short bx, short by)
{
    short dx = bx - ax, dy = by - ay, adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    short n = adx > ady ? adx : ady, sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1, i;
    short qx = 0, rx = n, qy = 0, ry = n, n2 = 2 * n;       /* numerators start at n: q = 0, r = n */
    if (n == 0) { pix(bits, ax, ay); return; }
    for (i = 0; i <= n; i++) {
        pix(bits, ax + sx * qx, ay + sy * qy);
        rx += 2 * adx; if (rx >= n2) { rx -= n2; qx++; }
        ry += 2 * ady; if (ry >= n2) { ry -= n2; qy++; }
    }
}

void rasterize(const unsigned char *xs, const unsigned char *ys, const unsigned char *up, short n,
               unsigned char *bits)
{
    static unsigned char gx[RMAXPTS], gy[RMAXPTS];
    short i, minx = 255, maxx = 0, miny = 255, maxy = 0, w, h, side, ox, oy;
    for (i = 0; i < 128; i++) bits[i] = 0;
    if (n <= 0) return;
    if (n > RMAXPTS) n = RMAXPTS;
    for (i = 0; i < n; i++) {
        if (xs[i] < minx) minx = xs[i];
        if (xs[i] > maxx) maxx = xs[i];
        if (ys[i] < miny) miny = ys[i];
        if (ys[i] > maxy) maxy = ys[i];
    }
    w = maxx - minx;
    h = maxy - miny;
    side = w > h ? w : h;
    if (side < MINSIDE) side = MINSIDE;
    ox = MARGIN + (E - rdiv((unsigned short)(w * E), side)) / 2;
    oy = MARGIN + (E - rdiv((unsigned short)(h * E), side)) / 2;
    for (i = 0; i < n; i++) {
        gx[i] = (unsigned char)(ox + rdiv((unsigned short)((xs[i] - minx) * E), side));
        gy[i] = (unsigned char)(oy + rdiv((unsigned short)((ys[i] - miny) * E), side));
    }
    for (i = 0; i < n; i++) {
        pix(bits, gx[i], gy[i]);
        if (i + 1 < n && !up[i]) line(bits, gx[i], gy[i], gx[i + 1], gy[i + 1]);
    }
}

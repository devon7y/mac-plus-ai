/* Rail Runner renderer: the C version of runner/tools/scene.py.
 *
 * Camera-relative depth zc = world z - zcam, zcam = player z - ZP. A point (X, Y, zc) is at
 *   x = PF_CX + F*X/zc,  y = PF_HOR + F*(CAM_H - Y)/zc.
 * Planes parallel to z map rows to one depth; edges parallel to z are lines through the
 * vanishing point, so roofs, sides and ramps are filled row by row with fixed-point stepping.
 */
#include <MacMemory.h>
#include "render.h"

Surface pf;
short dirty_top = 0;          /* smallest playfield row drawn by sprites this frame */
static short prev_dirty = 0;
short render_mask = 0x7FFF;   /* bench profiling: 1 poles 2 blocks 4 faces/ramps 8 coins 16 player */
static unsigned short *rowp[PF_H];

#define F PF_F
#define HOR PF_HOR
#define CX PF_CX
#define COIN_D 300
#define FACE_LOD_W 24                         /* faces narrower than this draw only their base */
#define PILLAR_FAR (10 * U)                   /* train window pillars only nearer than this */
#define COIN_FAR (ZP + 13 * U)
#define POLE_FAR (ZP + 18 * U)
#ifdef PROFILE
#define NOINL __attribute__((noinline))
long prof_cnt[8];   /* 0 fill_rect calls 1 fill_rect rows 2 vline calls 3 vline rows 4 train rows 5 sprite calls 6 sprite rows*words 7 draw_face calls */
#define PCNT(i, v) (prof_cnt[i] += (v))
#else
#define NOINL
#define PCNT(i, v)
#endif

/* 68000 signed 32/16 division (quotient must fit in 16 bits) */
static inline short divs(long a, short b)
{
    __asm__("divs.w %1,%0" : "+d"(a) : "dmi"(b) : "cc");
    return (short)a;
}

/* F*X/zc, X in zu, zc >= ZNEAR */
static inline short proj(short X, short zc)
{
    return divs((long)F * X, zc);
}

static void make_lane_images(void);
static unsigned char coin_size[64];   /* projected diameter -> largest coin size not above it */
#define RECIP_N ((DRAW_DIST + ZP + 2048) >> 3)
static short recip[RECIP_N];   /* (F << 14) / zc for zc = 8i + 4 */

void render_init(void)
{
    short y;
    pf.base = (unsigned short *)NewPtr((long)PF_RB * PF_H);
    pf.rw = PF_RB / 2;
    pf.w = PF_W;
    pf.h = PF_H;
    for (y = 0; y < PF_H; y++)
        rowp[y] = pf.base + (long)y * (PF_RB / 2);
    for (y = 0; y < RECIP_N; y++)
        recip[y] = (short)(((long)F << 14) / (y * 8 + 4));
    for (y = 0; y < 64; y++) {
        short i;
        for (i = N_COIN_SIZES - 1; i > 0 && coin_diam[i] > y; i--)
            ;
        coin_size[y] = (unsigned char)i;
    }
    make_lane_images();
}

/* ------------------------------------------------------------------ primitives */
#define ROWW (PF_RB / 2)
static const unsigned short lmask[16] = {
    0xFFFF, 0x7FFF, 0x3FFF, 0x1FFF, 0x0FFF, 0x07FF, 0x03FF, 0x01FF,
    0x00FF, 0x007F, 0x003F, 0x001F, 0x000F, 0x0007, 0x0003, 0x0001};
static const unsigned short rmask[16] = {
    0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00,
    0xFF80, 0xFFC0, 0xFFE0, 0xFFF0, 0xFFF8, 0xFFFC, 0xFFFE, 0xFFFF};
static const unsigned short bit[16] = {
    0x8000, 0x4000, 0x2000, 0x1000, 0x0800, 0x0400, 0x0200, 0x0100,
    0x0080, 0x0040, 0x0020, 0x0010, 0x0008, 0x0004, 0x0002, 0x0001};

/* fill [x0, x1] of a playfield row with p; requires 0 <= x0 <= x1 < PF_W */
static inline void hfill(unsigned short *row, short x0, short x1, unsigned short p)
{
    unsigned short *q = row + (x0 >> 4);
    short n = (x1 >> 4) - (x0 >> 4);
    unsigned short lm = lmask[x0 & 15], rm = rmask[x1 & 15];
    if (n == 0) {
        lm &= rm;
        *q = (*q & ~lm) | (p & lm);
        return;
    }
    *q = (*q & ~lm) | (p & lm);
    q++;
    while (--n > 0)
        *q++ = p;
    *q = (*q & ~rm) | (p & rm);
}

/* clipped span on the playfield */
static inline void pspan(short y, short x0, short x1, short pat)
{
    if (x0 > x1) {
        short t = x0; x0 = x1; x1 = t;
    }
    if (x0 < 0) x0 = 0;
    if (x1 >= PF_W) x1 = PF_W - 1;
    if (x0 > x1)
        return;
    hfill(rowp[y], x0, x1, fill_pats[(pat << 3) + (y & 7)]);
}

static inline void pblack(short y, short x)
{
    if ((unsigned short)x < PF_W)
        rowp[y][x >> 4] |= bit[x & 15];
}

/* 68000 loops in fast.S (all parameters 32-bit so the stack layout is fixed) */
extern void vl_or_l(unsigned short *p, long n, unsigned long m);
extern void vl_and_l(unsigned short *p, long n, unsigned long m);
extern void vl_or_w(unsigned short *p, long n, unsigned long m);
extern void vl_and_w(unsigned short *p, long n, unsigned long m);
extern void spr_rows(unsigned short *dst, const unsigned short *src, long rows, long ww, long dskip);
extern void trap_rows(unsigned short *row, long n, long a, long astep, long b, long bstep,
                      const unsigned short *pat8, long y);

/* vertical line w pixels wide (1..16), black (white if white != 0), clipped */
static void vline(short x, short y0, short y1, short w, short white)
{
    unsigned long m;
    unsigned short *p;
    short n, s;
    if (x < 0) { w += x; x = 0; }
    if (x + w > PF_W) w = PF_W - x;
    if (w <= 0) return;
    if (y0 < 0) y0 = 0;
    if (y1 >= PF_H) y1 = PF_H - 1;
    n = y1 - y0;
    if (n < 0) return;
    PCNT(2, 1); PCNT(3, n + 1);
    s = x & 15;
    m = (0xFFFFFFFFUL >> s) & ~(0xFFFFFFFFUL >> (s + w));
    p = rowp[y0] + (x >> 4);
    if ((x >> 4) == ROWW - 1) {
        unsigned short m16 = (unsigned short)(m >> 16);
        if (white) vl_and_w(p, n + 1, (unsigned short)~m16);
        else vl_or_w(p, n + 1, m16);
        return;
    }
    if (white) vl_and_l(p, n + 1, ~m);
    else vl_or_l(p, n + 1, m);
}

/* copy pixels [x0, x1] of a row from src (same layout); requires 0 <= x0 <= x1 < PF_W */
static inline void hcopy(unsigned short *dst, const unsigned short *src, short x0, short x1)
{
    short w0 = x0 >> 4, n = (x1 >> 4) - w0;
    unsigned short lm = lmask[x0 & 15], rm = rmask[x1 & 15];
    dst += w0;
    src += w0;
    if (n == 0) {
        lm &= rm;
        *dst = (*dst & ~lm) | (*src & lm);
        return;
    }
    *dst = (*dst & ~lm) | (*src & lm);
    dst++;
    src++;
    while (--n > 0)
        *dst++ = *src++;
    *dst = (*dst & ~rm) | (*src & rm);
}

/* generic (any surface) span: used by the HUD */
void fill_span(const Surface *s, short y, short x0, short x1, short pat)
{
    if ((unsigned short)y >= (unsigned short)s->h)
        return;
    if (x0 > x1) {
        short t = x0; x0 = x1; x1 = t;
    }
    if (x0 < 0) x0 = 0;
    if (x1 >= s->w) x1 = s->w - 1;
    if (x0 > x1)
        return;
    hfill(s->base + (long)y * s->rw, x0, x1, fill_pats[(pat << 3) + (y & 7)]);
}

/* rectangle on any surface (68000 loop in fast.S) */
/* all parameters are 32-bit so the stack layout is fixed (Retro68 pushes shorts as 2 bytes) */
extern void rect_rows(unsigned short *row, long stride, long n, long x0, long x1,
                      const unsigned short *pat8, long y);
extern void train_rows(unsigned short *dst, const unsigned short *src, long n,
                       long a, long astep, long b, long bstep);

void fill_rect(const Surface *s, short x0, short y0, short x1, short y1, short pat)
{
    if (x0 < 0) x0 = 0;
    if (x1 >= s->w) x1 = s->w - 1;
    if (y0 < 0) y0 = 0;
    if (y1 >= s->h) y1 = s->h - 1;
    if (x0 > x1 || y0 > y1)
        return;
    PCNT(0, 1); PCNT(1, y1 - y0 + 1);
    rect_rows(s->base + (long)y0 * s->rw, s->rw * 2, y1 - y0 + 1, x0, x1, fill_pats + (pat << 3), y0);
}

/* a face covering pixels [x0, x1) x [y0, y1), art in 1/64ths */
static void draw_face(const FaceRect *r, short x0, short y0, short x1, short y1)
{
    short w = x1 - x0, h = y1 - y0;
    const FaceRect *end = 0;
    if (w <= 0 || h <= 0 || x0 >= PF_W || x1 <= 0 || y0 >= PF_H || y1 <= 0)
        return;
    PCNT(7, 1);
    if (w < FACE_LOD_W)
        end = r + 1;                          /* small (far) faces: just the framed base rect */
    for (; r->kind != 255 && r != end; r++) {
        short a = x0 + (short)(((long)r->u0 * w) >> 6);
        short b = x0 + (short)(((long)r->u1 * w) >> 6) - 1;
        short c = y0 + (short)(((long)r->v0 * h) >> 6);
        short d = y0 + (short)(((long)r->v1 * h) >> 6) - 1;
        if (b < a || d < c)
            continue;
        if (r->kind == 1) {
            if (b - a >= 2 && d - c >= 2 && d - c < 12) {
                fill_rect(&pf, a, c, b, d, ART_BLACK);   /* short: black box, then the inside */
                fill_rect(&pf, a + 1, c + 1, b - 1, d - 1, r->pat);
            } else if (b - a >= 2 && d - c >= 2) {
                fill_rect(&pf, a, c, b, c, ART_BLACK);
                fill_rect(&pf, a, d, b, d, ART_BLACK);
                vline(a, c + 1, d - 1, 1, 0);
                vline(b, c + 1, d - 1, 1, 0);
                fill_rect(&pf, a + 1, c + 1, b - 1, d - 1, r->pat);
            } else {
                fill_rect(&pf, a, c, b, d, ART_BLACK);
            }
        } else {
            fill_rect(&pf, a, c, b, d, r->pat);
        }
    }
}

/* masked, pre-shifted sprite with its anchor at (x, y) */
void draw_sprite(const Surface *s, short id, short x, short y)
{
    const SprDef *d = &spr_defs[id];
    short left = x - d->ax, top = y - d->ay;
    short ns = d->nshift, k, w0, ww = d->ww, rows = d->h, c0 = 0, c1;
    const unsigned short *src;
    unsigned short *dst;
    short rw = s->rw, maxw = s->w >> 4;

    switch (ns) {                 /* pre-shift for this x: (left & 15) * ns / 16 */
    case 16: k = left & 15; break;
    case 8:  k = (left & 15) >> 1; break;
    case 4:  k = (left & 15) >> 2; break;
    default: k = ((left & 15) * ns) >> 4; break;
    }
    w0 = left >> 4;
    if (s == &pf && top < dirty_top)
        dirty_top = top < 0 ? 0 : top;
    src = spr_words + d->base + d->offs[k];
    if (top < 0) {
        if (-top >= rows)
            return;
        src += (long)(-top) * ww * 2;
        rows += top;
        top = 0;
    }
    if (top + rows > s->h)
        rows = s->h - top;
    if (rows <= 0)
        return;
    c1 = ww;
    if (w0 < 0) c0 = -w0;
    if (w0 + ww > maxw) c1 = maxw - w0;
    if (c0 >= c1)
        return;
    dst = (s == &pf ? rowp[top] : s->base + (long)top * rw) + w0;
    PCNT(5, 1); PCNT(6, (long)rows * ww);
    if (c0 == 0 && c1 == ww && ww <= 32) {
        spr_rows(dst, src, rows, ww, (long)(rw - ww) * 2);
    } else if (c0 == 0 && c1 == ww) {
        short skip = rw - ww;
        while (--rows >= 0) {
            short n = ww;
            while (--n >= 0) {
                *dst = (*dst & src[0]) | src[1];
                dst++;
                src += 2;
            }
            dst += skip;
        }
    } else {
        while (--rows >= 0) {
            short c;
            for (c = c0; c < c1; c++)
                dst[c] = (dst[c] & src[2 * c]) | src[2 * c + 1];
            src += 2 * ww;
            dst += rw;
        }
    }
}

void draw_number(const Surface *s, short x, short y, long v, short min_digits)
{
#ifdef HAVE_FONT_0
    char buf[10];
    short n = 0, w = spr_defs[SPR_FONT_0].w + 1;
    if (v < 0) v = 0;
    do {
        buf[n++] = (char)(v % 10);
        v /= 10;
    } while (v && n < 10);
    while (n < min_digits)
        buf[n++] = 0;
    while (n > 0) {
        draw_sprite(s, SPR_FONT_0 + buf[--n], x, y);
        x += w;
    }
#else
    (void)s; (void)x; (void)y; (void)v; (void)min_digits;
#endif
}

/* ------------------------------------------------------------------ background */
#if PF_RB == 48
#define MOVEM_REGS "%%d0-%%d7/%%a2-%%a5"
#elif PF_RB == 40
#define MOVEM_REGS "%%d0-%%d7/%%a2-%%a3"
#elif PF_RB == 32
#define MOVEM_REGS "%%d0-%%d7"
#else
#error "PF_W must be 256, 320 or 384"
#endif

/* copy n rows: row i from srcs[i] to dst + i * dst_step (movem.l, A5 saved and restored) */
static void copy_rows(const unsigned short *const *srcs, unsigned char *dst, short dst_step, short n)
{
    if (n <= 0)
        return;
    __asm__ volatile(
        "movem.l %%d2-%%d7/%%a2-%%a6,-(%%sp)\n\t"
        "move.l %1,%%a1\n\t"
        "move.w %2,-(%%sp)\n\t"
        "move.w %3,%%d0\n\t"
        "move.l %0,%%a6\n\t"
        "subq.w #1,%%d0\n\t"
        "move.w %%d0,-(%%sp)\n"
        "1:\n\t"
        "move.l (%%a6)+,%%a0\n\t"
        "movem.l (%%a0)," MOVEM_REGS "\n\t"
        "movem.l " MOVEM_REGS ",(%%a1)\n\t"
        "adda.w 2(%%sp),%%a1\n\t"
        "subq.w #1,(%%sp)\n\t"
        "bpl.s 1b\n\t"
        "addq.l #4,%%sp\n\t"
        "movem.l (%%sp)+,%%d2-%%d7/%%a2-%%a6\n\t"
        :
        : "a"(srcs), "a"(dst), "d"(dst_step), "d"(n)
        : "d0", "d1", "a0", "a1", "cc", "memory");
}

static const unsigned short *srcrows[PF_H];

/* copy n contiguous playfield rows from src to dst, dst rows dst_step bytes apart (movem.l) */
static void blit_rows(const unsigned short *src, unsigned char *dst, short dst_step, short n)
{
    if (n <= 0)
        return;
    __asm__ volatile(
        "movem.l %%d2-%%d7/%%a2-%%a6,-(%%sp)\n\t"
        "move.l %0,%%a0\n\t"
        "move.l %1,%%a1\n\t"
        "move.w %2,%%a6\n\t"
        "move.w %3,%%d0\n\t"
        "subq.w #1,%%d0\n\t"
        "move.w %%d0,-(%%sp)\n"
        "1:\n\t"
        "movem.l (%%a0)+," MOVEM_REGS "\n\t"
        "movem.l " MOVEM_REGS ",(%%a1)\n\t"
        "adda.w %%a6,%%a1\n\t"
        "subq.w #1,(%%sp)\n\t"
        "bpl.s 1b\n\t"
        "addq.l #2,%%sp\n\t"
        "movem.l (%%sp)+,%%d2-%%d7/%%a2-%%a6\n\t"
        :
        : "a"(src), "a"(dst), "d"(dst_step), "d"(n)
        : "d0", "d1", "a0", "a1", "cc", "memory");
}

void render_background(long zcam)
{
    short y, from = prev_dirty < PF_HOR + 1 ? prev_dirty : PF_HOR + 1;
    const unsigned short *b0 = (const unsigned short *)bg_rows;
    for (y = from; y < PF_H; y++) {
        const unsigned short *s = b0 + (long)y * ROWW;
        short z = row_sleeper_z[y];
        if (z && (((short)zcam + z) & (SLEEPER_P - 1)) < SLEEPER_LEN)
            s += (long)PF_H * ROWW;
        srcrows[y - from] = s;
    }
    copy_rows(srcrows, (unsigned char *)rowp[from], PF_RB, PF_H - from);
    dirty_top = PF_H;
}

/* ------------------------------------------------------------------ objects */
/* A box on the ground (train, wall block): roof, inner side, front face. */
static void draw_block(short xc, short zn, short zf, short height, const FaceRect *front,
                       const SideBand *bands, short roof_pat, short pillars)
{
    short hw = TRAIN_W / 2, zn_c = zn > ZNEAR ? zn : ZNEAR;
    short dh = CAM_H - height;
    short xl = xc - hw, xr = xc + hw, y, ya, yb;
    long fx0, fx1, sl, sr;
    if (zf <= zn_c)
        return;
    /* roof: rows of the plane Y = height, edges are lines through the vanishing point */
    if (!(render_mask & 32)) goto side;
    ya = HOR + divs((long)F * dh, zf);
    yb = HOR + divs((long)F * dh, zn_c);
    if (yb >= PF_H) yb = PF_H - 1;
    sl = ((long)xl << 16) / dh;
    sr = ((long)xr << 16) / dh;
    y = ya < HOR + 1 ? HOR + 1 : ya;
    fx0 = ((long)CX << 16) + sl * (y - HOR) + 0x8000;
    fx1 = ((long)CX << 16) + sr * (y - HOR) + 0x8000;
    if (y <= yb)
        pspan(y, (short)(fx0 >> 16), (short)(fx1 >> 16), ART_BLACK);
    {
        const unsigned short *pp = fill_pats + (roof_pat << 3);
        for (y = y + 1; y <= yb; y++) {
            short a, b;
            fx0 += sl;
            fx1 += sr;
            a = (short)(fx0 >> 16);
            b = (short)(fx1 >> 16);
            if (a < 0) a = 0;
            if (b >= PF_W) b = PF_W - 1;
            if (a <= b) {
                hfill(rowp[y], a, b, pp[y & 7]);
                pblack(y, (short)(fx0 >> 16));
                pblack(y, (short)(fx1 >> 16));
            }
        }
    }
side:
    /* inner side face (outer lanes only: the camera sees the side facing the middle) */
    if (xc != 0 && (render_mask & 64)) {
        short xs = xc < 0 ? xr : xl;
        short xn = CX + proj(xs, zn_c), xfar = CX + proj(xs, zf), lo, hi;
        short y_top = HOR + divs((long)F * dh, zf);
        short y_bot = HOR + divs((long)F * CAM_H, zn_c);
        long lf[6], ls[6];
        unsigned short bp[5];
        short nl = 0, i, rowk;
        if (xn < xfar) { lo = xn; hi = xfar; } else { lo = xfar; hi = xn; }
        if (lo < 0) lo = 0;
        if (hi >= PF_W) hi = PF_W - 1;
        if (y_bot >= PF_H) y_bot = PF_H - 1;
        if (y_top <= HOR) y_top = HOR + 1;
        ls[nl++] = ((long)xs << 16) / (CAM_H - bands[0].y_from);
        for (i = 0; bands[i].pat >= 0 && nl < 6; i++)
            ls[nl++] = ((long)xs << 16) / (CAM_H - bands[i].y_to);
        rowk = y_top - HOR;
        for (i = 0; i < nl; i++)
            lf[i] = ((long)CX << 16) + ls[i] * rowk + 0x8000;
        if (lo <= hi) {
            for (y = y_top; y <= y_bot; y++) {
                short xa = (short)(lf[0] >> 16), xb, x0;
                unsigned short *row = rowp[y];
                for (i = 0; i < nl - 1; i++)
                    bp[i] = fill_pats[(bands[i].pat << 3) + (y & 7)];
                x0 = xa;
                if (xa < lo) xa = lo;
                if (xa > hi) xa = hi;
                for (i = 1; i < nl; i++) {
                    xb = (short)(lf[i] >> 16);
                    if (xb < lo) xb = lo;
                    if (xb > hi) xb = hi;
                    if (xa < xb) hfill(row, xa, xb, bp[i - 1]);
                    else if (xb < xa) hfill(row, xb, xa, bp[i - 1]);
                    xa = xb;
                }
                if (x0 >= lo && x0 <= hi)
                    pblack(y, x0);
                for (i = 0; i < nl; i++)
                    lf[i] += ls[i];
            }
            vline(xfar, y_top, HOR + divs((long)F * CAM_H, zf), 1, 0);
        }
        if (pillars && (render_mask & 256)) {
            long z = (long)zn + U;
            while (z < zf) {
                if (z > zn_c) {
                    short x = CX + proj(xs, (short)z);
                    vline(x, HOR + divs((long)F * (CAM_H - 820), (short)z), HOR + divs((long)F * (CAM_H - 500), (short)z), 1, 1);
                }
                z += U;
            }
        }
    }
    /* front face */
    if (zn >= ZNEAR && (render_mask & 128)) {
        short x0 = CX + proj(xl, zn), x1 = CX + proj(xr, zn);
        short y0 = HOR + divs((long)F * dh, zn), y1 = HOR + divs((long)F * CAM_H, zn);
        draw_face(front, x0, y0, x1 + 1, y1 + 1);
    }
}

NOINL static void draw_ramp(short xc, short zn, short zf)
{
    short hw = TRAIN_W / 2, zn_c, yn_w, yN, yF, xlN, xrN, xlF, xrF, y;
    long z;
    if (zf <= ZNEAR)
        return;
    zn_c = zn > ZNEAR ? zn : ZNEAR;
    if ((long)zf - zn < 32768L)              /* (divs.w instead of a library division) */
        yn_w = divs((long)TRAIN_H * (zn_c - zn), (short)(zf - zn));
    else
        yn_w = (short)((long)TRAIN_H * (zn_c - zn) / (zf - zn));
    yN = HOR + divs((long)F * (CAM_H - yn_w), zn_c);
    yF = HOR + divs((long)F * (CAM_H - TRAIN_H), zf);
    xlN = CX + proj(xc - hw, zn_c);
    xrN = CX + proj(xc + hw, zn_c);
    xlF = CX + proj(xc - hw, zf);
    xrF = CX + proj(xc + hw, zf);
    if (yN <= yF)
        return;
    {
        long a = ((long)xlF << 16) + 0x8000, b = ((long)xrF << 16) + 0x8000;
        long sa = (((long)(xlN - xlF)) << 16) / (yN - yF), sb = (((long)(xrN - xrF)) << 16) / (yN - yF);
        short y0 = yF, y1 = yN < PF_H ? yN : PF_H - 1;
        if (y0 < 0) {
            a += sa * (long)(-y0);
            b += sb * (long)(-y0);
            y0 = 0;
        }
        if (y1 >= y0)                         /* rows filled by the 68000 loop in fast.S */
            trap_rows(rowp[y0], y1 - y0 + 1, a, sa, b, sb, fill_pats + (ART_RAMP_PAT << 3), y0);
    }
    for (z = zn + U / 2; z < zf; z += U / 2) {
        if (z > zn_c) {
            short Y = (long)zf - zn < 32768L ? divs((long)TRAIN_H * (z - zn), (short)(zf - zn))
                                             : (short)((long)TRAIN_H * (z - zn) / (zf - zn));
            y = HOR + divs((long)F * (CAM_H - Y), (short)z);
            if (y >= 0 && y < PF_H)
                pspan(y, CX + proj(xc - hw, (short)z), CX + proj(xc + hw, (short)z), ART_BLACK);
        }
    }
}

NOINL static void draw_face_obj(short xc, short z, short width, short height, const FaceRect *art)
{
    short x0, x1, y0, y1;
    if (z < ZNEAR)
        return;
    x0 = CX + proj(xc - width / 2, z);
    x1 = CX + proj(xc + width / 2, z);
    y0 = HOR + divs((long)F * (CAM_H - height), z);
    y1 = HOR + divs((long)F * CAM_H, z);
    draw_face(art, x0, y0, x1 + 1, y1 + 1);
}

NOINL static void draw_poles(long zcam)
{
    long z;
    if (zcam >= 0 && zcam < (long)POLE_SPACING * 32000)    /* one divs.w instead of library calls */
        z = ((long)divs(zcam, POLE_SPACING) + 1) * POLE_SPACING;
    else
        z = (zcam / POLE_SPACING + 1) * POLE_SPACING;
    short zs[8], n = 0, i, s;
    while (z - zcam < POLE_FAR && n < 8) {
        zs[n++] = (short)(z - zcam);
        z += POLE_SPACING;
    }
    for (i = n - 1; i >= 0; i--) {
        short zc = zs[i], w, ytop, ybot, yarm, th;
        if (zc < ZNEAR)
            continue;
        w = divs((long)F * POLE_W, zc);
        if (w < 1) w = 1;
        if (w > 16) w = 16;
        ytop = HOR + divs((long)F * (CAM_H - POLE_TOP), zc);
        ybot = HOR + divs((long)F * CAM_H, zc);
        yarm = HOR + divs((long)F * (CAM_H - POLE_ARM_Y), zc);
        th = divs((long)F * 40, zc);
        if (th < 1) th = 1;
        for (s = -1; s <= 1; s += 2) {
            short x = CX + s * proj(POLE_X, zc), xa = CX + s * proj(POLE_ARM_X, zc);
            vline(x - (w >> 1), ytop, ybot, w, 0);
            if (x < xa)
                fill_rect(&pf, x, yarm, xa, yarm + th - 1, ART_BLACK);
            else
                fill_rect(&pf, xa, yarm, x, yarm + th - 1, ART_BLACK);
        }
    }
}

/* ------------------------------------------------------------------ trains (fast path)
 * The roof and inner side of an endless train in a lane look the same whatever its ends, so
 * they are rendered once per lane and style at startup (procedurally, by draw_block) and each
 * frame copies, row by row, the part between the train's near and far depths. */
static unsigned short *limg[3][3];   /* style: 0 parked train, 1 oncoming train, 2 wall block */
static long tr_sl[3], tr_sr[3], tr_sb[3];   /* per-lane edge slopes (16.16 px per row) */
static short ttab[3][PF_H][4];   /* per lane and row: the endless train's span (see train_tab) */
extern void train_tab(unsigned short *dst, const unsigned short *src, long n, const short *tab);

static void make_train_tab(void)
{
    short lane, y;
    for (lane = 0; lane < 3; lane++) {
        long sA = lane == 2 ? tr_sb[2] : tr_sl[lane], sB = lane == 0 ? tr_sb[0] : tr_sr[lane];
        for (y = 0; y < PF_H; y++) {
            short *t = ttab[lane][y];
            long k = y - HOR;
            short x0 = (short)((((long)CX << 16) + sA * k + 0x8000) >> 16);
            short x1 = (short)((((long)CX << 16) + sB * k + 0x8000) >> 16);
            if (x0 < 0) x0 = 0;
            if (x1 >= PF_W) x1 = PF_W - 1;
            t[0] = t[1] = t[2] = 0;
            t[3] = -2;
            if (y <= HOR || x0 > x1)
                continue;
            t[0] = (x0 >> 4) * 2;
            t[1] = lmask[x0 & 15];
            t[2] = rmask[x1 & 15];
            t[3] = (x1 >> 4) - (x0 >> 4) - 1;
            if (t[3] < 0)
                t[1] &= t[2];
        }
    }
}

static void make_lane_images(void)
{
    short st, lane, y;
    unsigned short *save[PF_H];
    short mask = render_mask;
    for (y = 0; y < PF_H; y++)
        save[y] = rowp[y];
    for (lane = 0; lane < 3; lane++) {
        short xc = (lane - 1) * U, xl = xc - TRAIN_W / 2, xr = xc + TRAIN_W / 2;
        tr_sl[lane] = ((long)xl << 16) / (CAM_H - TRAIN_H);
        tr_sr[lane] = ((long)xr << 16) / (CAM_H - TRAIN_H);
        tr_sb[lane] = ((long)(lane == 0 ? xr : xl) << 16) / CAM_H;
    }
    make_train_tab();
    render_mask = 32 | 64;
    for (st = 0; st < 3; st++) {
        for (lane = 0; lane < 3; lane++) {
            unsigned short *img = (unsigned short *)NewPtrClear((long)PF_RB * PF_H);
            limg[st][lane] = img;
            if (!img)
                continue;
            for (y = 0; y < PF_H; y++)
                rowp[y] = img + (long)y * ROWW;
            draw_block((lane - 1) * U, ZNEAR, 32000, TRAIN_H, art_train_front,
                       st == 2 ? art_wall_side : st ? art_oncoming_side : art_train_side,
                       st == 2 ? ART_WALL_TOP : ART_ROOF_PAT, 0);
        }
    }
    for (y = 0; y < PF_H; y++)
        rowp[y] = save[y];
    render_mask = mask;
}

static void draw_train(short lane, short zn, short zf, short style)
{
    short xc = (lane - 1) * U, hw = TRAIN_W / 2, zn_c = zn > ZNEAR ? zn : ZNEAR;
    short dh = CAM_H - TRAIN_H, xl = xc - hw, xr = xc + hw, xs = lane == 0 ? xr : xl;
    short ya, yb, ygf, ybot, xn, xfar, k;
    long rl, rr, lb, sl, sr, sb;
    const unsigned short *img = limg[style][lane];
    if (zf <= zn_c || !img)
        return;
    ya = HOR + divs((long)F * dh, zf);
    yb = HOR + divs((long)F * dh, zn_c);
    ygf = HOR + divs((long)F * CAM_H, zf);
    ybot = lane == 1 ? yb : HOR + divs((long)F * CAM_H, zn_c);
    if (ya <= HOR) ya = HOR + 1;
    if (ybot >= PF_H) ybot = PF_H - 1;
    sl = tr_sl[lane];
    sr = tr_sr[lane];
    sb = tr_sb[lane];
    k = ya - HOR;
    rl = ((long)CX << 16) + sl * k + 0x8000;
    rr = ((long)CX << 16) + sr * k + 0x8000;
    lb = ((long)CX << 16) + sb * k + 0x8000;
    xn = CX + proj(xs, zn_c);
    xfar = CX + proj(xs, zf);
    /* rows split where an edge switches between a line (roof edge / ground line) and a
     * constant (the near or far end); each segment is one call to the 68000 loop */
    {
        short ys[4], ny = 0, i;
        ys[ny++] = ya;
        if (lane != 1) {
            if (yb + 1 > ya && yb + 1 <= ybot) ys[ny++] = yb + 1;
            if (ygf > ya && ygf <= ybot) {
                if (ny == 2 && ygf < ys[1]) { ys[2] = ys[1]; ys[1] = ygf; ny = 3; }
                else if (ny == 2 && ygf == ys[1]) ;
                else ys[ny++] = ygf;
            }
        }
        ys[ny] = ybot + 1;
        for (i = 0; i < ny; i++) {
            short y0 = ys[i], n = ys[i + 1] - y0;
            long dk = (long)(y0 - ya);
            long A, AS, B, BS;
            if (n <= 0)
                continue;
            if (lane == 0) {
                if (y0 <= yb) { A = rl + sl * dk; AS = sl; } else { A = (long)xn << 16; AS = 0; }
                if (y0 >= ygf) { B = lb + sb * dk; BS = sb; } else { B = (long)xfar << 16; BS = 0; }
            } else if (lane == 2) {
                if (y0 >= ygf) { A = lb + sb * dk; AS = sb; } else { A = (long)xfar << 16; AS = 0; }
                if (y0 <= yb) { B = rr + sr * dk; BS = sr; } else { B = (long)xn << 16; BS = 0; }
            } else {
                A = rl + sl * dk; AS = sl;
                B = rr + sr * dk; BS = sr;
            }
            PCNT(4, n);
            if (AS && BS)                     /* both edges are the lines: table rows */
                train_tab(rowp[y0], img + (long)y0 * ROWW, n, ttab[lane][y0]);
            else
                train_rows(rowp[y0], img + (long)y0 * ROWW, n, A, AS, B, BS);
        }
    }
    /* far end: top edge of the roof and the vertical edge of the side */
    if (ya < PF_H)        /* (a far end right in front of the camera projects below the playfield) */
        pspan(ya, CX + divs((long)xl * (ya - HOR), dh), CX + divs((long)xr * (ya - HOR), dh), ART_BLACK);
    if (lane != 1) {
        vline(xfar, ya, ygf < PF_H ? ygf : PF_H - 1, 1, 0);
        /* window pillars every 1 u (near ones only; positions from the reciprocal table) */
        if (style != 2) {
            long z = (long)zn + U;
            short zend = zf < PILLAR_FAR ? zf : PILLAR_FAR;
            if (z <= zn_c)
                z += ((zn_c - z) / U + 1) * U;
            while (z < zend) {
                short r = recip[(short)z >> 3];
                short x = CX + (short)(((long)xs * r) >> 14);
                vline(x, HOR + (short)(((long)(CAM_H - 820) * r) >> 14),
                      HOR + (short)(((long)(CAM_H - 500) * r) >> 14), 1, 1);
                z += U;
            }
        }
    }
    if (zn >= ZNEAR) {
        short x0 = CX + proj(xl, zn), x1 = CX + proj(xr, zn);
        short y0 = HOR + divs((long)F * dh, zn), y1 = HOR + divs((long)F * CAM_H, zn);
        draw_face(style == 2 ? art_wall_front : style ? art_oncoming_front : art_train_front,
                  x0, y0, x1 + 1, y1 + 1);
    }
}

/* ------------------------------------------------------------------ scene */
#define MAX_ITEMS (MAX_OBJ + MAX_COIN + 2)

static short lane_x(short lane) { return (lane - 1) * U; }

NOINL static void draw_coin(const Game *g, const Coin *c, short zc)
{
    short X = lane_x(c->lane), d, i, x, y, r;
    r = recip[zc >> 3];
    x = CX + (short)(((long)X * r) >> 14);
    y = HOR + (short)(((long)(short)(CAM_H - c->y) * r) >> 14);
    if (c->type == 0) {
        short frame;
        d = (short)(((long)COIN_D * r) >> 14);
        if (d <= 3) {
            if (y >= 1 && y < PF_H && x >= 0 && x < PF_W - 1) {
                rowp[y][x >> 4] |= bit[x & 15];
                rowp[y - 1][x >> 4] |= bit[x & 15];
                if (d == 3) {
                    rowp[y][(x + 1) >> 4] |= bit[(x + 1) & 15];
                    rowp[y - 1][(x + 1) >> 4] |= bit[(x + 1) & 15];
                }
            }
            return;
        }
        i = d < 64 ? coin_size[d] : coin_size[63];
        frame = (short)(((g->ticks >> 3) + (c->z >> 9)) & 3);
        draw_sprite(&pf, SPR_COIN0_0 + i * 4 + frame, x, y + (coin_diam[i] >> 1));
    } else {
#ifdef HAVE_PU_JETPACK_0
        /* scale relative to the player's depth: 0.25 0.35 0.5 0.7 1.0 */
        short s100 = divs((long)ZP * 100, zc), k;
        k = s100 >= 100 ? 4 : s100 >= 70 ? 3 : s100 >= 50 ? 2 : s100 >= 35 ? 1 : 0;
        if (s100 < 20) {
            draw_sprite(&pf, SPR_COIN0_0 + 6 * 4, x, y + 4);
        } else {
            short base = SPR_PU_JETPACK_0;
            switch (c->type) {
            case PU_SNEAKERS: base = SPR_PU_SNEAKERS_0; break;
            case PU_MAGNET:   base = SPR_PU_MAGNET_0; break;
            case PU_MULT:     base = SPR_PU_MULT_0; break;
            case PU_BOARD:    base = SPR_PU_BOARD_0; break;
            }
            draw_sprite(&pf, base + k, x, y + (spr_defs[base + k].h >> 1));
        }
#else
        draw_sprite(&pf, SPR_COIN0_0 + 12 * 4, x, y + 8);
#endif
    }
}

static short rex_sprite(const Game *g)
{
    const Player *p = &g->p;
    if (g->state == ST_CRASHED) {
#ifdef HAVE_REX_CRASH
        return SPR_REX_CRASH;
#endif
    }
    if (g->jet_t) {
#ifdef HAVE_REX_JET0
        return (g->ticks >> 2) & 1 ? SPR_REX_JET1 : SPR_REX_JET0;
#endif
    }
    if (p->roll) {
#ifdef HAVE_REX_ROLL0
        return (g->ticks >> 2) & 1 ? SPR_REX_ROLL1 : SPR_REX_ROLL0;
#endif
    }
    if (!p->gnd) {
#ifdef HAVE_REX_JUMP
        return p->vy > 0 ? SPR_REX_JUMP : SPR_REX_FALL;
#endif
    }
    if (g->hover_t) {
#ifdef HAVE_REX_BOARD0
        return (g->ticks >> 3) & 1 ? SPR_REX_BOARD1 : SPR_REX_BOARD0;
#endif
    }
    if (g->stumble_t || g->state == ST_CAUGHT) {
#ifdef HAVE_REX_STUMBLE
        return SPR_REX_STUMBLE;
#endif
    }
    if (p->sw) {
#ifdef HAVE_REX_LEFT
        return p->lane > p->frm ? SPR_REX_RIGHT : SPR_REX_LEFT;
#endif
    }
#ifdef HAVE_REX_RUN3
    return SPR_REX_RUN0 + (short)((g->z >> 9) & 3);
#else
    return SPR_REX_RUN0;
#endif
}

void render_player(const Game *g)
{
    const Player *p = &g->p;
    long xw = lane_x(p->lane) - divs((long)(short)(lane_x(p->lane) - lane_x(p->frm)) * p->sw, SWITCH_T);
    short x = CX + proj((short)xw, ZP);
    short y = HOR + divs((long)F * (CAM_H - p->y), ZP);
    if (g->invuln_t && (g->ticks & 4) && g->state == ST_RUN)
        return;                                   /* blink while invulnerable */
    draw_sprite(&pf, rex_sprite(g), x, y);
}

NOINL static void render_chasers(const Game *g)
{
#ifdef HAVE_GRUMBLE_RUN0
    const Player *p = &g->p;
    long xw = lane_x(p->lane) - divs((long)(short)(lane_x(p->lane) - lane_x(p->frm)) * p->sw, SWITCH_T);
    short x = CX + proj((short)xw, ZP);
    short sink, gy;
    short caught = g->state != ST_RUN;
    if (!caught && g->chase_t == 0)
        return;
    sink = 0;
    if (!caught && g->chase_t < 60)
        sink = (60 - g->chase_t) * 2;
    if (caught)
        sink = g->state_t < 30 ? 40 - g->state_t : 10;
    gy = PF_H + 18 + sink;
#ifdef HAVE_BISCUIT_RUN0
    draw_sprite(&pf, (g->ticks >> 3) & 1 ? SPR_BISCUIT_RUN1 : SPR_BISCUIT_RUN0, x + 44, PF_H - 2 + sink / 2);
#endif
    if (caught) {
#ifdef HAVE_GRUMBLE_GRAB
        draw_sprite(&pf, SPR_GRUMBLE_GRAB, x - 10, gy);
        return;
#endif
    }
    draw_sprite(&pf, (g->ticks >> 3) & 1 ? SPR_GRUMBLE_RUN1 : SPR_GRUMBLE_RUN0, x - 34, gy);
#else
    (void)g;
#endif
}

#define NB ((DRAW_DIST + ZP) / 256 + 2)
static short head[NB] __attribute__((aligned(4))), nxt[MAX_ITEMS], itm[MAX_ITEMS];

NOINL static void bucket(short n, short key, short item)
{
    short b = key >> 8;
    if (b < 0) b = 0;
    if (b >= NB) b = NB - 1;
    itm[n] = item;
    nxt[n] = head[b];
    head[b] = n;
}

NOINL static void draw_item(const Game *g, short item, long zcam)
{
    short kind = item >> 12, idx = item & 0x0FFF;
    if (kind == 1) {
        if (render_mask & 8) {
            const Coin *c = &g->coin[idx];
            draw_coin(g, c, (short)(c->z - zcam));
        }
        return;
    }
    {
        const Obj *o = &g->obj[idx];
        long zn = o->z0 - zcam, zf = o->z1 - zcam;
        short xc = lane_x(o->lane);
        if (zf > 32000) zf = 32000;
        if (zn < -32000) zn = -32000;
        if (!(render_mask & ((o->kind == K_RAMP || o->kind == K_LOW || o->kind == K_HIGH) ? 4 : 2)))
            return;
        switch (o->kind) {
        case K_TRAIN:
            draw_train(o->lane, (short)zn, (short)zf, 0);
            break;
        case K_ONCOMING:
            draw_train(o->lane, (short)zn, (short)zf, 1);
            break;
        case K_WALL:
            draw_train(o->lane, (short)zn, (short)zf, 2);
            break;
        case K_RAMP:
            draw_ramp(xc, (short)zn, (short)zf);
            break;
        case K_LOW:
            draw_face_obj(xc, (short)zn, ART_BARRIER_W, LOW_H, art_low_face);
            break;
        case K_HIGH:
            draw_face_obj(xc, (short)zn, ART_BARRIER_W, HIGH_T, art_high_face);
            break;
        }
    }
}

void render_objects(const Game *g)
{
    long zcam = g->z - ZP;
    short n = 0, i, b;
    {
        long *h = (long *)head;          /* (NB is even: clear two heads per long) */
        for (b = 0; b < NB / 2; b++)
            h[b] = -1L;
        if (NB & 1)
            head[NB - 1] = -1;
    }
    for (i = 0; i < g->nobj; i++) {
        const Obj *o = &g->obj[i];
        long zn = o->z0 - zcam, zf = o->z1 - zcam;
        short key;
        if (zn > DRAW_DIST + ZP || zf < ZNEAR)
            continue;
        key = (short)(zn < ZNEAR ? ZNEAR : zn);
        if ((o->kind == K_TRAIN || o->kind == K_ONCOMING || o->kind == K_WALL || o->kind == K_RAMP) &&
            zn < ZP && zf > ZP)
            key = ZP + 256;                       /* alongside or under the player: draw first */
        bucket(n++, key, i);
    }
    for (i = 0; i < g->ncoin; i++) {
        long zc = g->coin[i].z - zcam;
        if (zc > COIN_FAR || zc < ZP - 600)
            continue;
        bucket(n++, (short)zc, 0x1000 | i);
    }
    if (render_mask & 1)
        draw_poles(zcam);
    for (b = NB - 1; b >= 0; b--) {
        for (i = head[b]; i >= 0; i = nxt[i])
            draw_item(g, itm[i], zcam);
        if (b == (ZP >> 8) && (render_mask & 16))
            render_player(g);
    }
    if (render_mask & 16)
        render_chasers(g);
}

void render_frame(const Game *g)
{
    render_background(g->z - ZP);
    render_objects(g);
}

void blit_to_screen(unsigned char *dst, short dst_rb)
{
    /* rows above the horizon only change when a sprite was drawn there (this or last frame) */
    short from = dirty_top < prev_dirty ? dirty_top : prev_dirty;
    if (from > PF_HOR + 1)
        from = PF_HOR + 1;
    blit_rows(rowp[from], dst + (long)from * dst_rb, dst_rb, PF_H - from);
    prev_dirty = dirty_top < PF_HOR + 1 ? dirty_top : PF_HOR + 1;
}

void render_invalidate(void)
{
    prev_dirty = 0;
    dirty_top = 0;
}

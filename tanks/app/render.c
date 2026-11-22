/* Tank Duel renderer. See render.h.
 *
 * Dirty rectangles are kept per drawn object (a tank with its turret, a shell, an effect, the
 * crosshair, ...): at the start of a frame each object's previous rectangle is restored from the
 * background; at the end the union of its previous and current rectangle goes to the screen. */
#include <MacMemory.h>
#include <Quickdraw.h>
#include "render.h"
#include "../../toolchain/lib/plus.h"

unsigned char *r_fb, *r_bg, *r_screen;
short r_srb;
Rect r_arena_rect, r_hud_rect;
GrafPort r_port;

typedef struct { short c0, c1, y0, y1; } DRect;   /* word columns [c0, c1), rows [y0, y1); c1 = 0: empty */
static DRect prev[R_NOBJ], cur[R_NOBJ];
static short obj, full_now, force_full = 1;
static PlView arena_view;

short r_init(short top, short bottom)
{
    Rect b = qd.screenBits.bounds;
    short left, h = AH + HUD_H, y;
    if (b.right - b.left < AW || bottom - top < h)
        return -1;
    left = b.left + ((b.right - b.left - AW) / 2 & ~15);
    y = top + (bottom - top - h) / 2;
    SetRect(&r_hud_rect, left, y, left + AW, y + HUD_H);
    SetRect(&r_arena_rect, left, y + HUD_H, left + AW, y + h);
    r_srb = qd.screenBits.rowBytes & 0x3FFF;
    r_screen = (unsigned char *)qd.screenBits.baseAddr + (long)(r_arena_rect.top - b.top) * r_srb
               + ((left - b.left) >> 3);
    arena_view.base = r_screen;
    arena_view.rowBytes = r_srb;
    arena_view.w = AW;
    arena_view.h = AH;
    arena_view.rect = r_arena_rect;
    r_fb = (unsigned char *)NewPtrClear((long)AH * FBRB);
    r_bg = (unsigned char *)NewPtrClear((long)AH * FBRB);
    if (!r_fb || !r_bg)
        return -1;
    OpenPort(&r_port);
    r_port.portBits.baseAddr = (Ptr)r_fb;
    r_port.portBits.rowBytes = FBRB;
    SetRect(&r_port.portBits.bounds, 0, 0, AW, AH);
    SetRect(&r_port.portRect, 0, 0, AW, AH);
    RectRgn(r_port.visRgn, &r_port.portRect);
    RectRgn(r_port.clipRgn, &r_port.portRect);
    TextFont(0);
    return 0;
}

static unsigned short *bgw(short x, short y)
{
    return (unsigned short *)(r_bg + (long)y * FBRB + ((x >> 4) << 1));
}

void r_draw_arena(const unsigned char map[18][24])
{
    short tx, ty, r;
#define W(x, y) ((x) < 0 || (y) < 0 || (x) >= 24 || (y) >= 18 || map[y][x])
    for (ty = 0; ty < 18; ty++)
        for (tx = 0; tx < 24; tx++) {
            const unsigned short *t;
            if (map[ty][tx]) {
                short m = (!W(tx, ty - 1) ? 1 : 0) | (!W(tx + 1, ty) ? 2 : 0) | (!W(tx, ty + 1) ? 4 : 0)
                          | (!W(tx - 1, ty) ? 8 : 0);
                t = WALL_TILE[m];
            } else {
                short m = (W(tx - 1, ty) ? 1 : 0) | (W(tx, ty - 1) ? 2 : 0) | (W(tx - 1, ty - 1) ? 4 : 0);
                t = FLOOR_TILE[m];
            }
            for (r = 0; r < 16; r++)
                *bgw(tx * 16, ty * 16 + r) = t[r];
        }
#undef W
    BlockMoveData(r_bg, r_fb, (long)AH * FBRB);
    force_full = 1;
}

void r_obj(short id)
{
    obj = id;
}

void r_dirty(short x0, short y0, short x1, short y1)
{
    DRect *d = &cur[obj];
    short c0, c1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > AW) x1 = AW;
    if (y1 > AH) y1 = AH;
    if (x1 <= x0 || y1 <= y0)
        return;
    c0 = x0 >> 4;
    c1 = (x1 + 15) >> 4;
    if (!d->c1) {
        d->c0 = c0;
        d->c1 = c1;
        d->y0 = y0;
        d->y1 = y1;
        return;
    }
    if (c0 < d->c0) d->c0 = c0;
    if (c1 > d->c1) d->c1 = c1;
    if (y0 < d->y0) d->y0 = y0;
    if (y1 > d->y1) d->y1 = y1;
}

/* copy `words` words of `rows` rows (rows of FBRB bytes in the source, drb in the destination):
   a jump into an unrolled run of move.w (a0)+,(a1)+ per row */
static void copy_rect(const void *src, void *dst, short drb, short words, short rows)
{
    register const void *a0 __asm__("a0") = src;
    register void *a1 __asm__("a1") = dst;
    short back = words * 2, ss = FBRB - words * 2, ds = drb - words * 2;
    if (words <= 0 || rows <= 0)
        return;
    rows--;
    __asm__ volatile(
        "lea    2f(%%pc),%%a2\n\t"
        "suba.w %[back],%%a2\n"
        "1:\tjmp   (%%a2)\n\t"
        ".rept 26\n\t"
        "move.w (%%a0)+,(%%a1)+\n\t"
        ".endr\n"
        "2:\tadda.w %[ss],%%a0\n\t"
        "adda.w %[ds],%%a1\n\t"
        "dbra   %[rows],1b"
        : "+a"(a0), "+a"(a1), [rows] "+d"(rows)
        : [back] "d"(back), [ss] "d"(ss), [ds] "d"(ds)
        : "a2", "memory", "cc");
}

static void restore(const DRect *r)
{
    long off = (long)r->y0 * FBRB + (r->c0 << 1);
    copy_rect(r_bg + off, r_fb + off, FBRB, r->c1 - r->c0, r->y1 - r->y0);
}

static void show(const DRect *r)
{
    short c1 = r->c1 > AW / 16 ? AW / 16 : r->c1;
    copy_rect(r_fb + (long)r->y0 * FBRB + (r->c0 << 1), r_screen + (long)r->y0 * r_srb + (r->c0 << 1), r_srb,
              c1 - r->c0, r->y1 - r->y0);
}

void r_begin(short full)
{
    short k;
    full_now = full || force_full;
    force_full = 0;
    obj = 0;
    if (full_now)
        BlockMoveData(r_bg, r_fb, (long)AH * FBRB);
    else
        for (k = 0; k < R_NOBJ; k++)
            if (prev[k].c1)
                restore(&prev[k]);
    for (k = 0; k < R_NOBJ; k++)
        cur[k].c1 = 0;
}

void r_spr(short idx, short cx, short cy)
{
    const Spr *s = &SPR[idx];
    short x0 = cx - s->ox, y0 = cy - s->oy, sh = x0 & 15, st, h = s->h;
    const unsigned short *d = s->d;
    unsigned char *base = r_fb + (long)y0 * FBRB + ((x0 >> 4) << 1);
    for (st = 0; st < s->nstrip; st++) {
        register const unsigned short *dp __asm__("a0") = d;
        register unsigned char *p __asm__("a1") = base + (st << 1);
        short n = h - 1;
        /* per row: the long at d is mask:image; shift both, and.l / or.l into the frame */
        __asm__ volatile(
            "1:\tmove.l (%[d])+,%%d0\n\t"
            "move.w %%d0,%%d1\n\t"
            "clr.w  %%d0\n\t"
            "lsr.l  %[sh],%%d0\n\t"
            "swap   %%d1\n\t"
            "clr.w  %%d1\n\t"
            "lsr.l  %[sh],%%d1\n\t"
            "not.l  %%d0\n\t"
            "and.l  %%d0,(%[p])\n\t"
            "or.l   %%d1,(%[p])\n\t"
            "lea    52(%[p]),%[p]\n\t"
            "dbra   %[n],1b"
            : [d] "+a"(dp), [p] "+a"(p), [n] "+d"(n)
            : [sh] "d"(sh)
            : "d0", "d1", "cc", "memory");
        d += 2 * h;
    }
    r_dirty(x0, y0, x0 + 16 * s->nstrip + 16, y0 + h);
}

static void plot(short x, short y)
{
    if ((unsigned short)x < AW && (unsigned short)y < AH)
        r_fb[(long)y * FBRB + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
}

void r_line(short x0, short y0, short x1, short y1, short dash)
{
    short dx = x1 - x0, dy = y1 - y0, sx = 1, sy = FBRB, err, n, k;
    unsigned char *p, m;
    {
        short a = x0 < x1 ? x0 : x1, b = x0 < x1 ? x1 : x0, c = y0 < y1 ? y0 : y1, d = y0 < y1 ? y1 : y0;
        r_dirty(a, c, b + 1, d + 1);
        if (a < 0 || c < 0 || b >= AW || d >= AH) {      /* partly off the arena: plot with checks */
            n = (b - a > d - c ? b - a : d - c);
            for (k = 0; k <= n; k++)
                if (!dash || ((k >> (dash - 1)) & 1) == 0)
                    plot(x0 + (short)((long)(x1 - x0) * k / (n ? n : 1)),
                         y0 + (short)((long)(y1 - y0) * k / (n ? n : 1)));
            return;
        }
    }
    if (dx < 0) { dx = -dx; sx = -1; }
    if (dy < 0) { dy = -dy; sy = -FBRB; }
    p = r_fb + (long)y0 * FBRB + (x0 >> 3);
    m = (unsigned char)(0x80 >> (x0 & 7));
    n = dx > dy ? dx : dy;
    err = dx - dy;
    for (k = 0; k <= n; k++) {
        if (!dash || ((k >> (dash - 1)) & 1) == 0)
            *p |= m;
        {
            short e2 = err << 1;
            if (e2 > -dy) {
                err -= dy;
                if (sx > 0) {
                    if (!(m >>= 1)) { m = 0x80; p++; }
                } else {
                    if (!(m <<= 1)) { m = 1; p--; }
                }
            }
            if (e2 < dx) {
                err += dx;
                p += sy;
            }
        }
    }
}

void r_box(short x0, short y0, short x1, short y1)
{
    Rect r;
    GrafPtr old;
    GetPort(&old);
    SetPort(&r_port);
    SetRect(&r, x0, y0, x1, y1);
    EraseRect(&r);
    FrameRect(&r);
    InsetRect(&r, 2, 2);
    FrameRect(&r);
    SetPort(old);
    r_dirty(x0, y0, x1, y1);
}

void r_logo(short x, short y)
{
    short r, k, bpr = LOGO_W / 8;
    const unsigned char *s = LOGO;
    for (r = 0; r < LOGO_H; r++) {
        unsigned char *d = r_fb + (long)(y + r) * FBRB + (x >> 3);
        for (k = 0; k < bpr; k++)
            d[k] = *s++;
    }
    r_dirty(x, y, x + LOGO_W, y + LOGO_H);
}

void r_commit_bg(void)
{
    BlockMoveData(r_fb, r_bg, (long)AH * FBRB);
    force_full = 1;
}

void r_present_all(void)
{
    pl_present_rows(&arena_view, r_fb, FBRB, 0, AH);
}

void r_present(void)
{
    short k;
    if (full_now)
        r_present_all();
    else
        for (k = 0; k < R_NOBJ; k++) {
            DRect u = cur[k];
            const DRect *p = &prev[k];
            if (p->c1) {
                if (!u.c1)
                    u = *p;
                else {
                    if (p->c0 < u.c0) u.c0 = p->c0;
                    if (p->c1 > u.c1) u.c1 = p->c1;
                    if (p->y0 < u.y0) u.y0 = p->y0;
                    if (p->y1 > u.y1) u.y1 = p->y1;
                }
            }
            if (u.c1)
                show(&u);
        }
    for (k = 0; k < R_NOBJ; k++)
        prev[k] = cur[k];
}

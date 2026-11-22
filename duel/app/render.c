/* Orbit Duel renderer. See render.h. */
#include <Quickdraw.h>
#include <MacMemory.h>
#include "plus.h"
#include "render.h"
#include "art.h"

#define PAD 16
#define BW (ARENA_W + 2 * PAD)          /* 480 px */
#define BH (ARENA_H + 2 * PAD)          /* 368 rows */
#define RB (BW / 8)                     /* 60 bytes per buffer row */
#define AT(buf, x, y) ((buf) + (long)((y) + PAD) * RB + (((x) + PAD) >> 3))

static uint8_t *stars, *bg, *fb;
static PlView view;
static int16_t top_limit;               /* first arena row that may be presented */
int16_t r_view_left, r_view_top;
long r_rects_last;

/* ---- dirty rectangles (arena rows y0..y1-1, words w0..w1-1) ----------------------------- */
typedef struct { int16_t y0, y1, w0, w1; } DR;
#define MAXR 200
static DR rA[MAXR], rB[MAXR];
static DR *cur = rA, *prev = rB;
static int16_t ncur, nprev, full_cur, full_prev;

/* the list add_rect appends to: normally the current frame's */
static DR *tl;
static int16_t *tn, *tf;

static void add_rect(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    if (x0 < 0) {                                   /* split what wraps around the edges */
        add_rect(x0 + ARENA_W, y0, x1 <= 0 ? x1 + ARENA_W : ARENA_W, y1);
        if (x1 <= 0) return;
        x0 = 0;
    }
    if (x1 > ARENA_W) {
        add_rect(x0 >= ARENA_W ? x0 - ARENA_W : 0, y0, x1 - ARENA_W, y1);
        if (x0 >= ARENA_W) return;
        x1 = ARENA_W;
    }
    if (y0 < 0) {
        add_rect(x0, y0 + ARENA_H, x1, y1 <= 0 ? y1 + ARENA_H : ARENA_H);
        if (y1 <= 0) return;
        y0 = 0;
    }
    if (y1 > ARENA_H) {
        add_rect(x0, y0 >= ARENA_H ? y0 - ARENA_H : 0, x1, y1 - ARENA_H);
        if (y0 >= ARENA_H) return;
        y1 = ARENA_H;
    }
    if (x0 >= x1 || y0 >= y1)
        return;
    if (*tn >= MAXR) {
        *tf = 1;
        return;
    }
    tl[*tn].y0 = y0;
    tl[*tn].y1 = y1;
    tl[*tn].w0 = x0 >> 4;
    tl[*tn].w1 = (x1 + 15) >> 4;
    (*tn)++;
}

static void copy_words(uint8_t *dst, int16_t drb, const uint8_t *src, int16_t srb, int16_t rows, int16_t nw)
{
    while (rows-- > 0) {
        uint16_t *d = (uint16_t *)dst;
        const uint16_t *s = (const uint16_t *)src;
        int16_t n = nw;
        while (n-- > 0)
            *d++ = *s++;
        dst += drb;
        src += srb;
    }
}

static void restore(const DR *r)            /* bg -> fb */
{
    copy_words(AT(fb, r->w0 * 16, r->y0), RB, AT(bg, r->w0 * 16, r->y0), RB, r->y1 - r->y0, r->w1 - r->w0);
}

static void present(const DR *r)            /* fb -> screen */
{
    int16_t y0 = r->y0 < top_limit ? top_limit : r->y0;
    if (y0 >= r->y1)
        return;
    copy_words(view.base + (long)y0 * view.rowBytes + r->w0 * 2, view.rowBytes,
               AT(fb, r->w0 * 16, y0), RB, r->y1 - y0, r->w1 - r->w0);
}

/* ---- setup ----------------------------------------------------------------------------- */
static int8_t px8[24], py8[24];             /* explosion particle directions x speed (1/8 px/step) */

void r_init(void)
{
    int16_t i;
    stars = (uint8_t *)NewPtrClear((long)RB * BH);
    bg = (uint8_t *)NewPtrClear((long)RB * BH);
    fb = (uint8_t *)NewPtrClear((long)RB * BH);
    pl_view_init(&view, ARENA_W, ARENA_H);
    tl = cur; tn = &ncur; tf = &full_cur;
    r_view_left = view.rect.left;
    r_view_top = view.rect.top;
    for (i = 0; i < 24; i++) {
        int16_t a = (int16_t)((i * 256 / 24 + ((i * 37) & 7)) & 255);
        int16_t spd = 5 + (i * 53) % 15;
        px8[i] = (int8_t)(((int32_t)COS_Q14(a) * spd) >> 14);
        py8[i] = (int8_t)(-(((int32_t)SIN_Q14(a) * spd) >> 14));
    }
}

void r_set_top_limit(int16_t screen_row)
{
    top_limit = screen_row - view.rect.top;
    if (top_limit < 0)
        top_limit = 0;
}

/* ---- pixels and lines ------------------------------------------------------------------ */
static void plot(uint8_t *buf, int16_t x, int16_t y)       /* white pixel (clipped to the buffer) */
{
    if ((uint16_t)(x + PAD) < BW && (uint16_t)(y + PAD) < BH)
        *AT(buf, x, y) &= ~(0x80 >> ((x + PAD) & 7));
}

static void plot_wrap(int16_t x, int16_t y)                /* white pixel in fb, wrapping */
{
    while (x < 0) x += ARENA_W;
    while (x >= ARENA_W) x -= ARENA_W;
    while (y < 0) y += ARENA_H;
    while (y >= ARENA_H) y -= ARENA_H;
    *AT(fb, x, y) &= ~(0x80 >> ((x + PAD) & 7));
}

static void line(uint8_t *buf, int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t thick)
{
    int16_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int16_t dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int16_t err = dx + dy, e2;
    for (;;) {
        plot(buf, x0, y0);
        if (thick) {
            plot(buf, x0 + 1, y0);
            plot(buf, x0, y0 + 1);
            plot(buf, x0 + 1, y0 + 1);
        }
        if (x0 == x1 && y0 == y1)
            break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ---- bg editing ------------------------------------------------------------------------ */
static void clip(int16_t *x0, int16_t *y0, int16_t *x1, int16_t *y1)
{
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > ARENA_W) *x1 = ARENA_W;
    if (*y1 > ARENA_H) *y1 = ARENA_H;
}

/* op 0: copy stars, 1: black, 2: invert; pixel exact */
static void region(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t op)
{
    int16_t y, b0, b1, b;
    uint8_t lm, rm;
    clip(&x0, &y0, &x1, &y1);
    if (x0 >= x1 || y0 >= y1)
        return;
    b0 = (x0 + PAD) >> 3;
    b1 = (x1 - 1 + PAD) >> 3;
    lm = 0xFF >> ((x0 + PAD) & 7);
    rm = 0xFF << (7 - ((x1 - 1 + PAD) & 7));
    for (y = y0; y < y1; y++) {
        uint8_t *d = bg + (long)(y + PAD) * RB, *s = stars + (long)(y + PAD) * RB;
        for (b = b0; b <= b1; b++) {
            uint8_t m = 0xFF;
            if (b == b0) m &= lm;
            if (b == b1) m &= rm;
            if (op == 0) d[b] = (d[b] & ~m) | (s[b] & m);
            else if (op == 1) d[b] |= m;
            else d[b] ^= m;
        }
    }
}

void r_bg_clear(int16_t x0, int16_t y0, int16_t x1, int16_t y1) { region(x0, y0, x1, y1, 0); }
void r_bg_black(int16_t x0, int16_t y0, int16_t x1, int16_t y1) { region(x0, y0, x1, y1, 1); }
void r_bg_invert(int16_t x0, int16_t y0, int16_t x1, int16_t y1) { region(x0, y0, x1, y1, 2); }

void r_bg_frame(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    line(bg, x0, y0, x1 - 1, y0, 0);
    line(bg, x0, y1 - 1, x1 - 1, y1 - 1, 0);
    line(bg, x0, y0, x0, y1 - 1, 0);
    line(bg, x1 - 1, y0, x1 - 1, y1 - 1, 0);
}

void r_stars(uint32_t seed)
{
    long i, n = (long)RB * BH;
    uint32_t r = seed;
    int16_t k;
    for (i = 0; i < n; i++)
        stars[i] = 0xFF;
    for (k = 0; k < 150; k++) {
        int16_t x, y, dx, dy;
        r = r * 1103515245UL + 12345UL;
        x = (int16_t)((r >> 16) % ARENA_W);
        r = r * 1103515245UL + 12345UL;
        y = (int16_t)((r >> 16) % ARENA_H);
        dx = x - STAR_X;
        dy = y - STAR_Y;
        if ((int32_t)dx * dx + (int32_t)dy * dy < 40L * 40)
            continue;
        plot(stars, x, y);
        if (k % 17 == 0) {                          /* a few brighter ones */
            plot(stars, x - 1, y);
            plot(stars, x + 1, y);
            plot(stars, x, y - 1);
            plot(stars, x, y + 1);
        }
    }
    for (i = 0; i < n; i++)
        bg[i] = stars[i];
}

/* ---- vector text ------------------------------------------------------------------------ */
int16_t r_text_w(const char *s, int16_t scale)
{
    int16_t n = 0;
    while (s[n])
        n++;
    return n ? n * 6 * scale - 2 * scale : 0;
}

int16_t r_text(const char *s, int16_t x, int16_t y, int16_t scale, int16_t thick)
{
    int16_t x00 = x;
    for (; *s; s++, x += 6 * scale) {
        int16_t c = (uint8_t)*s, lx = 0, ly = 0, pen = 0;
        const uint8_t *g;
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (c < 32 || c > 95)
            c = '?';
        for (g = font_data + font_index[c - 32]; *g != 0xFE; g++) {
            int16_t px, py;
            if (*g == 0xFF) {
                pen = 0;
                continue;
            }
            px = x + (*g >> 4) * scale;
            py = y + (*g & 15) * scale;
            if (pen)
                line(bg, lx, ly, px, py, thick);
            else if (g[1] == 0xFE || g[1] == 0xFF)    /* a single point: a dot */
                line(bg, px, py, px, py, scale > 1 ? 1 : thick);
            lx = px;
            ly = py;
            pen = 1;
        }
    }
    return x - x00 - 2 * scale;
}

void r_text_c(const char *s, int16_t y, int16_t scale, int16_t thick)
{
    r_text(s, (ARENA_W - r_text_w(s, scale)) / 2, y, scale, thick);
}

/* ---- QuickDraw into bg ------------------------------------------------------------------ */
static GrafPort qport;
static int16_t qport_open;
static GrafPtr qold;

void r_qd_begin(void)
{
    BitMap bm;
    GetPort(&qold);
    if (!qport_open) {
        OpenPort(&qport);
        qport_open = 1;
    }
    SetPort(&qport);
    bm.baseAddr = (Ptr)AT(bg, 0, 0);
    bm.rowBytes = RB;
    SetRect(&bm.bounds, 0, 0, ARENA_W, ARENA_H);
    SetPortBits(&bm);
    PortSize(ARENA_W, ARENA_H);
    RectRgn(qport.visRgn, &bm.bounds);
    ClipRect(&bm.bounds);
    TextFont(0);
    TextSize(12);
    TextMode(srcBic);
    PenNormal();
    PenMode(patBic);
}

void r_qd_end(void)
{
    SetPort(qold);
}

/* ---- presenting ------------------------------------------------------------------------- */
void r_bg_dirty(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    /* goes on the list of the frame already shown, so the next r_begin restores it from bg
       and the next r_end presents it */
    tl = prev; tn = &nprev; tf = &full_prev;
    add_rect(x0, y0, x1, y1);
    tl = cur; tn = &ncur; tf = &full_cur;
}

void r_full(void)
{
    long i, n = (long)RB * BH;
    int16_t y, top = view.rect.top + top_limit;
    Rect b = qd.screenBits.bounds;
    short srb = qd.screenBits.rowBytes & 0x3FFF;
    for (i = 0; i < n; i++)
        fb[i] = bg[i];
    for (y = top; y < b.bottom; y++) {              /* black all round the arena */
        uint32_t *p = (uint32_t *)((uint8_t *)qd.screenBits.baseAddr + (long)(y - b.top) * srb);
        int16_t k;
        for (k = 0; k < (b.right - b.left) / 32; k++)
            p[k] = 0xFFFFFFFFUL;
    }
    pl_present_rows(&view, AT(fb, 0, 0), RB, top_limit, ARENA_H);
    ncur = nprev = 0;
    full_cur = full_prev = 0;
}

void r_begin(void)
{
    int16_t i;
    if (full_prev) {
        long k, n = (long)RB * BH;
        for (k = 0; k < n; k++)
            fb[k] = bg[k];
        return;
    }
    for (i = 0; i < nprev; i++)
        restore(&prev[i]);
}

void r_end(void)
{
    int16_t i;
    DR *t;
    if (full_prev || full_cur) {
        pl_present_rows(&view, AT(fb, 0, 0), RB, top_limit, ARENA_H);
        r_rects_last = -1;
    } else {
        for (i = 0; i < nprev; i++)
            present(&prev[i]);
        for (i = 0; i < ncur; i++)
            present(&cur[i]);
        r_rects_last = nprev + ncur;
    }
    t = prev; prev = cur; cur = t;
    nprev = ncur; ncur = 0;
    full_prev = full_cur; full_cur = 0;
    tl = cur; tn = &ncur; tf = &full_cur;
}

/* ---- objects ---------------------------------------------------------------------------- */
static void spr_ship(int16_t x, int16_t y, const uint16_t (*s)[2])   /* top-left x, y */
{
    int16_t bx = x + PAD, sh = 16 - (bx & 15), r;
    uint8_t *p = fb + (long)(y + PAD) * RB + ((bx >> 4) << 1);
    for (r = 0; r < 16; r++, p += RB) {
        uint32_t *q = (uint32_t *)p;
        *q = (*q | ((uint32_t)s[r][0] << sh)) & ~((uint32_t)s[r][1] << sh);
    }
}

static void spr_white(int16_t x, int16_t y, const uint16_t *s)
{
    int16_t bx = x + PAD, sh = 16 - (bx & 15), r;
    uint8_t *p = fb + (long)(y + PAD) * RB + ((bx >> 4) << 1);
    for (r = 0; r < 16; r++, p += RB)
        if (s[r])
            *(uint32_t *)p &= ~((uint32_t)s[r] << sh);
}

/* draw a 16x16 cell with its top-left at (x, y), plus its copies across the edges */
static void cell(int16_t x, int16_t y, const uint16_t (*ship)[2], const uint16_t *white)
{
    int16_t xs[2], ys[2], nx = 1, ny = 1, i, j;
    xs[0] = x;
    ys[0] = y;
    if (x < 0) xs[nx++] = x + ARENA_W;
    else if (x > ARENA_W - 16) xs[nx++] = x - ARENA_W;
    if (y < 0) ys[ny++] = y + ARENA_H;
    else if (y > ARENA_H - 16) ys[ny++] = y - ARENA_H;
    for (i = 0; i < nx; i++)
        for (j = 0; j < ny; j++) {
            if (xs[i] <= -16 || xs[i] >= ARENA_W || ys[j] <= -16 || ys[j] >= ARENA_H)
                continue;
            if (ship)
                spr_ship(xs[i], ys[j], ship);
            else
                spr_white(xs[i], ys[j], white);
        }
    add_rect(x, y, x + 16, y + 16);
}

static uint32_t fx_rng;
static int16_t fx_rand(int16_t n)
{
    fx_rng = fx_rng * 1103515245UL + 12345UL;
    return (int16_t)((fx_rng >> 16) % n);
}

static void star_glow(int16_t frame)
{
    static const int8_t disc[][2] = {{0,0},{1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,1},{1,-1},{-1,-1},
                                     {2,0},{-2,0},{0,2},{0,-2},{2,1},{-2,1},{2,-1},{-2,-1},{1,2},{-1,2},{1,-2},{-1,-2}};
    int16_t i;
    fx_rng = 0x51ED + (uint32_t)frame * 2654435761UL;
    for (i = 0; i < (int16_t)(sizeof disc / sizeof disc[0]); i++)
        plot(fb, STAR_X + disc[i][0], STAR_Y + disc[i][1]);
    for (i = 0; i < 10; i++) {                      /* flickering rays */
        int16_t a = (int16_t)((i * 26 + fx_rand(15) - 7 + (frame & 3) * 6) & 255);
        int16_t len = 5 + fx_rand(9);
        int16_t x1 = STAR_X + (int16_t)(((int32_t)COS_Q14(a) * len) >> 14);
        int16_t y1 = STAR_Y - (int16_t)(((int32_t)SIN_Q14(a) * len) >> 14);
        int16_t x0 = STAR_X + (int16_t)(((int32_t)COS_Q14(a) * 3) >> 14);
        int16_t y0 = STAR_Y - (int16_t)(((int32_t)SIN_Q14(a) * 3) >> 14);
        line(fb, x0, y0, x1, y1, 0);
    }
    for (i = 0; i < 6; i++)                         /* a few sparks around it */
        plot(fb, STAR_X + fx_rand(29) - 14, STAR_Y + fx_rand(29) - 14);
    add_rect(STAR_X - 15, STAR_Y - 15, STAR_X + 16, STAR_Y + 16);
}

static void explosion(int16_t cx, int16_t cy, int16_t t)
{
    int16_t i, n, f, R;
    if (t < 6) {                                    /* the flash: a disc, then a ring */
        int16_t r = 2 + t * 2, r2 = r * r, in2 = t >= 3 ? (r - 2) * (r - 2) : -1, dx, dy;
        for (dy = -r; dy <= r; dy++)
            for (dx = -r; dx <= r; dx++) {
                int16_t d = dx * dx + dy * dy;
                if (d <= r2 && d > in2)
                    plot_wrap(cx + dx, cy + dy);
            }
        add_rect(cx - r - 1, cy - r - 1, cx + r + 2, cy + r + 2);
    }
    if (t >= 45)
        return;
    f = t * (100 - t) / 100;                        /* distance travelled, slowing down */
    n = 24 - t / 2;
    for (i = 0; i < n; i++)
        plot_wrap(cx + ((px8[i] * f) >> 3), cy + ((py8[i] * f) >> 3));
    R = (int16_t)((19 * f) >> 3) + 2;
    add_rect(cx - R, cy - R, cx + R + 1, cy + R + 1);
}

static void sparkle(int16_t cx, int16_t cy, int16_t r, int16_t spin)
{
    int16_t i;
    for (i = 0; i < 8; i++) {
        int16_t a = (int16_t)((i * 32 + spin) & 255);
        plot_wrap(cx + (int16_t)(((int32_t)COS_Q14(a) * r) >> 14), cy - (int16_t)(((int32_t)SIN_Q14(a) * r) >> 14));
    }
    add_rect(cx - r - 1, cy - r - 1, cx + r + 2, cy + r + 2);
}

void r_world(const Sim *s, int16_t frame)
{
    int16_t i, k;
    star_glow(frame);
    for (i = 0; i < 2; i++)
        for (k = 0; k < MAX_TORPS; k++) {
            const Torp *t = &s->torp[i][k];
            int16_t x, y;
            if (!t->life)
                continue;
            x = (int16_t)(t->x >> FP);
            y = (int16_t)(t->y >> FP);
            plot_wrap(x, y);
            plot_wrap(x - 1, y);
            plot_wrap(x + 1, y);
            plot_wrap(x, y - 1);
            plot_wrap(x, y + 1);
            add_rect(x - 1, y - 1, x + 2, y + 2);
        }
    for (i = 0; i < 2; i++) {
        const Ship *p = &s->ship[i];
        int16_t x = (int16_t)(p->x >> FP), y = (int16_t)(p->y >> FP), rot = ((p->angle + 4) >> 3) & 31;
        if (!p->alive) {
            explosion(x, y, p->dead_t);
            continue;
        }
        if (p->hyper) {
            if (p->hyper > HYPER_T - 10)
                sparkle(x, y, (p->hyper - (HYPER_T - 10)) * 2, p->hyper * 8);
            continue;
        }
        if (p->jumps && p->hyper_cool > HYPER_COOL - 10)
            sparkle(x, y, (HYPER_COOL - p->hyper_cool) * 2 + 2, p->hyper_cool * 8);
        if (p->thrust)
            cell(x + flame_off[rot][0] - 8, y + flame_off[rot][1] - 8, 0, flame_spr[(frame >> 1) & 1][rot]);
        cell(x - 8, y - 8, ship_spr[i][rot], 0);
    }
}

void r_glow(int16_t frame)
{
    star_glow(frame);
}

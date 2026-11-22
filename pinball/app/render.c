/*
 * Neural Pinball renderer.
 *
 * Screen layout (512 x 342, or 448 wide with Narrow Screen): menu bar on top, then the table
 * (256 x 322) and the side panel (192 x 322) side by side, centred; anything left over is
 * cabinet pattern.
 *
 * The table is drawn from three 1-bit buffers of 32 x 322 bytes:
 *   bg    the playfield art (const, from tools/art.py)
 *   base  bg with the lamps that are on XORed in (lamps are XOR patches, so order is free)
 *   frm   base plus the moving parts (flippers, plunger, spinner, balls)
 * Each frame only the rectangles that changed are rebuilt (base -> frm, sprites drawn over)
 * and copied to the screen, so a frame costs a few hundred bytes of copying, not 10 KB.
 */
#include <Quickdraw.h>
#include <MacMemory.h>
#include "render.h"

#define TW8 32                       /* table row bytes */
#define PW8 24                       /* panel row bytes */
#define TOP 20                       /* menu bar height */

short comp_x, scr_w, scr_h;
static unsigned char *scr;           /* screen pixel (0, 0) */
static short srb;
static unsigned char *base, *frm, *pbuf;
static uint32_t lamps_drawn;
static int16_t face_drawn;
static long copied;

typedef struct { short x0, x1, y0, y1; } R8;   /* bytes [x0,x1), rows [y0,y1) */
#define MAXDIRTY 48
static R8 dirty[MAXDIRTY];
static short ndirty;

/* sprite state drawn last frame */
static short flip_f[2] = {-1, -1}, plunger_f = -1, spinner_f = -1;
static short ball_x[MAX_BALLS], ball_y[MAX_BALLS], ball_on[MAX_BALLS];

static short pdirty0 = 32767, pdirty1 = -1;   /* panel rows to copy */

long render_stats(void)
{
    return copied;
}

static void add_dirty(short x0, short x1, short y0, short y1)
{
    if (x0 < 0) x0 = 0;
    if (x1 > TW8) x1 = TW8;
    if (y0 < 0) y0 = 0;
    if (y1 > TABLE_H) y1 = TABLE_H;
    if (x0 >= x1 || y0 >= y1)
        return;
    if (ndirty >= MAXDIRTY) {             /* overflow: merge into the last one */
        R8 *r = &dirty[MAXDIRTY - 1];
        if (x0 < r->x0) r->x0 = x0;
        if (x1 > r->x1) r->x1 = x1;
        if (y0 < r->y0) r->y0 = y0;
        if (y1 > r->y1) r->y1 = y1;
        return;
    }
    dirty[ndirty].x0 = x0;
    dirty[ndirty].x1 = x1;
    dirty[ndirty].y0 = y0;
    dirty[ndirty].y1 = y1;
    ndirty++;
}

static int16_t touches(short x0, short x1, short y0, short y1)
{
    short i;
    for (i = 0; i < ndirty; i++)
        if (x0 < dirty[i].x1 && x1 > dirty[i].x0 && y0 < dirty[i].y1 && y1 > dirty[i].y0)
            return 1;
    return 0;
}

static void xor_patch(const Patch *p)
{
    short r, b;
    const unsigned char *s = p->bits;
    for (r = 0; r < p->h; r++) {
        unsigned char *d = base + (long)(p->y + r) * TW8 + p->x8;
        for (b = 0; b < p->w8; b++)
            d[b] ^= *s++;
    }
    add_dirty(p->x8, p->x8 + p->w8, p->y, p->y + p->h);
}

static void draw_sprite(const Sprite *s)
{
    short r, b;
    const unsigned char *m = s->mask, *im = s->img;
    for (r = 0; r < s->h; r++) {
        unsigned char *d = frm + (long)(s->y + r) * TW8 + s->x8;
        for (b = 0; b < s->w8; b++) {
            d[b] = (unsigned char)((d[b] & ~*m) | *im);
            m++;
            im++;
        }
    }
}

static void draw_ball(short x, short y)
{
    /* the 10 x 10 ball sprite at top-left (x, y), shifted into up to 3 bytes */
    short r, x8 = x >> 3, sh = x & 7;
    for (r = 0; r < BALL_D; r++) {
        short yy = y + r;
        unsigned long m, im;
        unsigned char *d;
        if (yy < 0 || yy >= TABLE_H)
            continue;
        m = ((unsigned long)ball_mask[r] << 8) >> sh;
        im = ((unsigned long)ball_img[r] << 8) >> sh;
        d = frm + (long)yy * TW8 + x8;
        if (x8 >= 0 && x8 < TW8)
            d[0] = (unsigned char)((d[0] & ~(m >> 16)) | (im >> 16));
        if (x8 + 1 >= 0 && x8 + 1 < TW8)
            d[1] = (unsigned char)((d[1] & ~(m >> 8)) | (im >> 8));
        if (x8 + 2 >= 0 && x8 + 2 < TW8)
            d[2] = (unsigned char)((d[2] & ~m) | im);
    }
}

static void copy_rect(const R8 *r, const unsigned char *src, unsigned char *dst)
{
    short y, w = r->x1 - r->x0, x0 = r->x0;
    long off = (long)r->y0 * TW8 + x0;
    for (y = r->y0; y < r->y1; y++, off += TW8) {
        const unsigned char *s = src + off;
        unsigned char *d = dst + off;
        short b = 0;
        if (x0 & 1) {
            *d++ = *s++;
            b = 1;
        }
        for (; b + 1 < w; b += 2, s += 2, d += 2)
            *(unsigned short *)d = *(const unsigned short *)s;
        if (b < w)
            *d = *s;
    }
}

static void to_screen(const R8 *r)
{
    short y, w = r->x1 - r->x0;
    unsigned char *s0 = frm + r->x0;
    unsigned char *d0 = scr + (long)(TOP + r->y0) * srb + (comp_x >> 3) + r->x0;
    for (y = r->y0; y < r->y1; y++) {
        const unsigned char *s = s0 + (long)y * TW8;
        unsigned char *d = d0;
        short b;
        if (!((long)s & 1) && !((long)d & 1)) {
            for (b = 0; b + 1 < w; b += 2)
                *(unsigned short *)(d + b) = *(const unsigned short *)(s + b);
            if (b < w)
                d[b] = s[b];
        } else
            for (b = 0; b < w; b++)
                d[b] = s[b];
        d0 += srb;
    }
    copied += (long)w * (r->y1 - r->y0);
}

void render_init(void)
{
    long i;
    scr = (unsigned char *)qd.screenBits.baseAddr;
    srb = qd.screenBits.rowBytes & 0x3FFF;
    scr_w = qd.screenBits.bounds.right - qd.screenBits.bounds.left;
    scr_h = qd.screenBits.bounds.bottom - qd.screenBits.bounds.top;
    comp_x = (short)(((scr_w - 448) / 2) & ~15);
    if (comp_x < 0)
        comp_x = 0;
    base = (unsigned char *)NewPtr((long)TW8 * TABLE_H);
    frm = (unsigned char *)NewPtr((long)TW8 * TABLE_H);
    pbuf = (unsigned char *)NewPtr((long)PW8 * TABLE_H);
    for (i = 0; i < (long)TW8 * TABLE_H; i++)
        base[i] = frm[i] = bg_bits[i];
    for (i = 0; i < (long)PW8 * TABLE_H; i++)
        pbuf[i] = panel_bits[i];
    lamps_drawn = 0;
    face_drawn = FACE_OFF;
}

static void sprite_rect_dirty(const Sprite *s)
{
    add_dirty(s->x8, s->x8 + s->w8, s->y, s->y + s->h);
}

static short flip_frame(const Flipper *f)
{
    static long mult[2], span_seen[2];
    long span = f->a_up - f->a_rest, d = f->ang - f->a_rest;
    short k, i = f->dir > 0;
    if (span == 0)
        return 0;
    if (span != span_seen[i]) {              /* frames per unit of angle, computed once */
        span_seen[i] = span;
        mult[i] = ((long)(NFLIPFRAMES - 1) << 16) / (span >> 8);
    }
    k = (short)((((d >> 8) * mult[i]) + 32768L) >> 16);
    if (k < 0) k = 0;
    if (k > NFLIPFRAMES - 1) k = NFLIPFRAMES - 1;
    return k;
}

static void frame_inner(const Game *g)
{
    short i, k;
    const World *w = &g->w;
    short nf[2], np, ns;
    short bx[MAX_BALLS], by[MAX_BALLS], bon[MAX_BALLS];
    /* lamps and the robot face */
    if (g->lamps != lamps_drawn) {
        uint32_t d = g->lamps ^ lamps_drawn;
        for (i = 0; i < NLAMPS; i++)
            if (d & (1UL << i))
                xor_patch(&lamp_patch[i]);
        lamps_drawn = g->lamps;
    }
    if (g->face != face_drawn) {
        if (face_drawn != FACE_OFF)
            xor_patch(&face_patch[face_drawn]);
        if (g->face != FACE_OFF)
            xor_patch(&face_patch[g->face]);
        face_drawn = g->face;
    }
    /* moving parts */
    for (i = 0; i < 2; i++) {
        nf[i] = flip_frame(&w->flip[i]);
        if (nf[i] != flip_f[i]) {                 /* frames have their own rectangles */
            if (flip_f[i] >= 0)
                sprite_rect_dirty(&flip_sprite[i][flip_f[i]]);
            sprite_rect_dirty(&flip_sprite[i][nf[i]]);
        }
    }
    np = (short)(w->plunger.pull >> 16);
    if (np > NPLUNGER - 1) np = NPLUNGER - 1;
    if (np != plunger_f)
        sprite_rect_dirty(&plunger_sprite[np]);
    ns = (short)((g->spinner_ang >> 6) & 3);
    if (ns != spinner_f)
        sprite_rect_dirty(&spinner_sprite[ns]);
    for (i = 0; i < MAX_BALLS; i++) {
        const Ball *b = &w->ball[i];
        bon[i] = b->state != BALL_OFF && b->y < ((fx)TABLE_H << 16);
        bx[i] = (short)((b->x + 32768L) >> 16) - BALL_D / 2;
        by[i] = (short)((b->y + 32768L) >> 16) - BALL_D / 2;
        if (bon[i] != ball_on[i] || (bon[i] && (bx[i] != ball_x[i] || by[i] != ball_y[i]))) {
            if (ball_on[i])
                add_dirty(ball_x[i] >> 3, (ball_x[i] + BALL_D + 7) >> 3, ball_y[i], ball_y[i] + BALL_D);
            if (bon[i])
                add_dirty(bx[i] >> 3, (bx[i] + BALL_D + 7) >> 3, by[i], by[i] + BALL_D);
        }
    }
    if (!ndirty)
        return;
    /* rebuild the dirty rectangles: background, then every sprite that touches one */
    for (k = 0; k < ndirty; k++)
        copy_rect(&dirty[k], base, frm);
    {
        const Sprite *s = &spinner_sprite[ns];
        if (touches(s->x8, s->x8 + s->w8, s->y, s->y + s->h))
            draw_sprite(s);
        s = &plunger_sprite[np];
        if (touches(s->x8, s->x8 + s->w8, s->y, s->y + s->h))
            draw_sprite(s);
        for (i = 0; i < 2; i++) {
            s = &flip_sprite[i][nf[i]];
            if (touches(s->x8, s->x8 + s->w8, s->y, s->y + s->h))
                draw_sprite(s);
        }
    }
    for (i = 0; i < MAX_BALLS; i++)
        if (bon[i] && touches(bx[i] >> 3, (bx[i] + BALL_D + 7) >> 3, by[i], by[i] + BALL_D))
            draw_ball(bx[i], by[i]);
    for (k = 0; k < ndirty; k++)
        to_screen(&dirty[k]);
    for (i = 0; i < 2; i++)
        flip_f[i] = nf[i];
    plunger_f = np;
    spinner_f = ns;
    for (i = 0; i < MAX_BALLS; i++) {
        ball_on[i] = bon[i];
        ball_x[i] = bx[i];
        ball_y[i] = by[i];
    }
}

void render_frame(const Game *g)
{
    ndirty = 0;
    frame_inner(g);
}

/* ------------------------------------------------------------------ panel */

static const unsigned char *glyph(char ch)
{
    static const char order[] = FONT_ORDER;
    short i;
    if (ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 32);
    for (i = 0; order[i]; i++)
        if (order[i] == ch)
            return font57[i];
    return font57[0];
}

/* OR (ink 1) or clear (ink 0) a run of up to 16 bits (left-aligned in `bits`) at pixel x of
   panel row y */
static void prow(short x, short y, unsigned short bits, short ink)
{
    unsigned char *d;
    unsigned long v;
    short sh;
    if (y < 0 || y >= TABLE_H || !bits)
        return;
    d = pbuf + (long)y * PW8 + (x >> 3);
    sh = x & 7;
    v = ((unsigned long)bits << 8) >> sh;          /* 24-bit window starting at byte x >> 3 */
    if (ink) {
        d[0] |= (unsigned char)(v >> 16);
        d[1] |= (unsigned char)(v >> 8);
        if ((x >> 3) + 2 < PW8) d[2] |= (unsigned char)v;
    } else {
        d[0] &= (unsigned char)~(v >> 16);
        d[1] &= (unsigned char)~(v >> 8);
        if ((x >> 3) + 2 < PW8) d[2] &= (unsigned char)~v;
    }
}

/* 1x text into the panel buffer; ink 1 black, 0 white */
static void ptext(short x, short y, const char *s, short ink)
{
    short i, r;
    for (i = 0; s[i]; i++, x += 6) {
        const unsigned char *g = glyph(s[i]);
        if (x < 0 || x > PANEL_W - 6)
            continue;
        for (r = 0; r < 7; r++)
            prow(x, (short)(y + r), (unsigned short)(g[r] << 11), ink);
    }
}

/* large dot-matrix text: each font pixel is a 2 x 2 white dot on a 3 px grid. dotrow[v] is
   the 15-pixel pattern of a 5-bit glyph row, left-aligned in 16 bits. */
static unsigned short dotrow[32];

static void pdots(short x, short y, const char *s)
{
    short i, r;
    if (!dotrow[1]) {
        short v, c;
        for (v = 0; v < 32; v++) {
            unsigned short p = 0;
            for (c = 0; c < 5; c++)
                if (v & (0x10 >> c))
                    p |= (unsigned short)(0xC000 >> (c * 3));
            dotrow[v] = p;
        }
    }
    for (i = 0; s[i]; i++, x += 18) {
        const unsigned char *g = glyph(s[i]);
        if (x < 0 || x > PANEL_W - 15)
            continue;
        for (r = 0; r < 7; r++) {
            unsigned short p = dotrow[g[r] & 31];
            prow(x, (short)(y + r * 3), p, 0);
            prow(x, (short)(y + r * 3 + 1), p, 0);
        }
    }
}

static void prect(short x0, short y0, short x1, short y1, short ink)
{
    short y, b, b0 = x0 >> 3, b1 = (x1 - 1) >> 3;
    unsigned char lm = (unsigned char)(0xFF >> (x0 & 7)), rm = (unsigned char)(0xFF << (7 - ((x1 - 1) & 7)));
    for (y = y0; y < y1; y++) {
        unsigned char *d = pbuf + (long)y * PW8;
        if (b0 == b1) {
            unsigned char m = (unsigned char)(lm & rm);
            if (ink) d[b0] |= m; else d[b0] &= (unsigned char)~m;
            continue;
        }
        if (ink) d[b0] |= lm; else d[b0] &= (unsigned char)~lm;
        for (b = (short)(b0 + 1); b < b1; b++)
            d[b] = ink ? 0xFF : 0;
        if (ink) d[b1] |= rm; else d[b1] &= (unsigned char)~rm;
    }
}

static void pmark(short y0, short y1)
{
    if (y0 < pdirty0) pdirty0 = y0;
    if (y1 > pdirty1) pdirty1 = y1;
}

static short slen(const char *s)
{
    short n = 0;
    while (s[n]) n++;
    return n;
}

static char last_dmd[4][28];
static short last_big = -1;

static int16_t same(const char *a, const char *b)
{
    short i;
    for (i = 0; a[i] || b[i]; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void keep(char *d, const char *s)
{
    short i;
    for (i = 0; s[i] && i < 26; i++)
        d[i] = s[i];
    d[i] = 0;
}

/* The score display: three lines, each redrawn only when it changes. */
void panel_update(const Game *g, const char *top, const char *mid, const char *low, int16_t big)
{
    short x0 = 4, x1 = PANEL_W - 4, n;
    (void)g;
    if (!same(top, last_dmd[0])) {
        keep(last_dmd[0], top);
        prect(x0, DMD_Y - 4, x1, DMD_Y + 10, 1);
        ptext(PANEL_W / 2 - (slen(top) * 6 - 1) / 2, DMD_Y + 1, top, 0);
        pmark(DMD_Y - 4, DMD_Y + 10);
    }
    if (big && big == last_big && slen(mid) == slen(last_dmd[1]) && !same(mid, last_dmd[1]) &&
        slen(mid) * 18 - 3 <= PANEL_W - 12) {
        /* same length (a score going up): redraw only the characters that changed */
        short i, x = PANEL_W / 2 - (slen(mid) * 18 - 3) / 2;
        for (i = 0; mid[i]; i++, x += 18)
            if (mid[i] != last_dmd[1][i]) {
                char one[2];
                one[0] = mid[i];
                one[1] = 0;
                prect(x, DMD_Y + 16, x + 15, DMD_Y + 37, 1);
                pdots(x, DMD_Y + 16, one);
            }
        keep(last_dmd[1], mid);
        pmark(DMD_Y + 16, DMD_Y + 37);
    } else if (!same(mid, last_dmd[1]) || big != last_big) {
        keep(last_dmd[1], mid);
        last_big = big;
        prect(x0, DMD_Y + 12, x1, DMD_Y + 40, 1);
        n = slen(mid);
        if (big && n * 18 - 3 <= PANEL_W - 12)
            pdots(PANEL_W / 2 - (n * 18 - 3) / 2, DMD_Y + 16, mid);
        else
            ptext(PANEL_W / 2 - (n * 6 - 1) / 2, DMD_Y + 23, mid, 0);
        pmark(DMD_Y + 12, DMD_Y + 40);
    }
    if (!same(low, last_dmd[2])) {
        keep(last_dmd[2], low);
        prect(x0, DMD_Y + 44, x1, DMD_Y + DMD_H + 4, 1);
        ptext(PANEL_W / 2 - (slen(low) * 6 - 1) / 2, DMD_Y + 50, low, 0);
        pmark(DMD_Y + 44, DMD_Y + DMD_H + 4);
    }
}

/* The info plate: the robot judge and up to 12 lines; only what changed is redrawn. */
static char info_last[12][28];
static short info_face = -1;

void panel_info(const char *const *lines, int16_t n, int16_t face)
{
    short i;
    if (face != info_face) {
        const Patch *j = &judge_img[face];
        for (i = 0; i < j->h; i++) {
            short b;
            for (b = 0; b < j->w8; b++)
                pbuf[(long)(j->y + i) * PW8 + j->x8 + b] = j->bits[i * j->w8 + b];
        }
        pmark(j->y, j->y + j->h);
        info_face = face;
    }
    for (i = 0; i < 12; i++) {
        const char *t = i < n ? lines[i] : "";
        short y = i < 6 ? INFO_Y + 8 + i * 10 : INFO_Y + 74 + (i - 6) * 10;
        short x = i < 6 ? 84 : 12;
        if (same(t, info_last[i]))
            continue;
        keep(info_last[i], t);
        prect(x, y - 1, PANEL_W - 9, y + 8, 0);
        ptext(x, y, t, 1);
        pmark(y - 1, y + 8);
    }
}

void panel_present(void)
{
    short y;
    if (pdirty1 < 0)
        return;
    for (y = pdirty0; y < pdirty1; y++) {
        const unsigned char *s = pbuf + (long)y * PW8;
        unsigned char *d = scr + (long)(TOP + y) * srb + (comp_x >> 3) + TW8;
        short b;
        for (b = 0; b < PW8; b += 2)
            *(unsigned short *)(d + b) = *(const unsigned short *)(s + b);
    }
    copied += (long)PW8 * (pdirty1 - pdirty0);
    pdirty0 = 32767;
    pdirty1 = -1;
}

void render_full(const Game *g)
{
    short y, b;
    /* margins outside the composite: cabinet pattern */
    for (y = TOP; y < scr_h; y++) {
        unsigned char *d = scr + (long)y * srb;
        unsigned char v = (unsigned char)((y & 1) ? 0xEE : 0xBB);
        for (b = 0; b < (comp_x >> 3); b++)
            d[b] = v;
        for (b = (short)((comp_x >> 3) + TW8 + PW8); b < (scr_w >> 3); b++)
            d[b] = v;
    }
    /* the whole table is dirty: everything is rebuilt and every sprite drawn */
    ndirty = 0;
    add_dirty(0, TW8, 0, TABLE_H);
    frame_inner(g);
    pmark(0, TABLE_H);
    panel_present();
}

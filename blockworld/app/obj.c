/* Blockworld: objects that are not cubes, and the sprite drawer (depth-tested against the runs). */
#include <string.h>
#include "obj.h"
#include "tables.h"
#include "world.h"

Obj objs[NOBJ];
static unsigned char ncol[WX * WX];      /* objects in each column: most have none (fast obj_at) */

static unsigned char *col_of(short x, short z)
{
    static unsigned char none;
    if (x < 0 || x >= WX || z < 0 || z >= WX) { none = 0; return &none; }
    return &ncol[x * WX + z];
}

void obj_reindex(void)
{
    short i;
    memset(ncol, 0, sizeof ncol);
    for (i = 0; i < NOBJ; i++)
        if (objs[i].type) (*col_of(objs[i].x, objs[i].z))++;
}

short obj_at(short x, short y, short z)
{
    short i;
    if (!*col_of(x, z)) return -1;
    for (i = 0; i < NOBJ; i++)
        if (objs[i].type && objs[i].x == x && objs[i].y == y && objs[i].z == z) return i;
    return -1;
}

short obj_add(short x, short y, short z, unsigned char type, unsigned char state)
{
    short i;
    for (i = 0; i < NOBJ; i++)
        if (!objs[i].type) {
            objs[i].x = x; objs[i].y = y; objs[i].z = z; objs[i].type = type; objs[i].state = state;
            (*col_of(x, z))++;
            return i;
        }
    return -1;
}

void obj_remove(short i)
{
    if (i >= 0 && i < NOBJ && objs[i].type) {
        unsigned char *c = col_of(objs[i].x, objs[i].z);
        if (*c) (*c)--;
        objs[i].type = O_NONE;
    }
}

/* ---- pictures: '#' black, 'o' white, '.' clear ---------------------------------------------- */

static const char *const torch_pic[16] = {
    "..##..", ".#oo#.", "#oo#o#", "#o##o#", ".#oo#.", "..##..", "..#o#.", "..#o#.",
    "..#o#.", "..#o#.", "..#o#.", "..#o#.", "..#o#.", "..#o#.", "..#o#.", "..###."};
static const char *const sapling_pic[12] = {
    "....##......", "...#oo#.##..", "..#oooo#oo#.", "..#oo##ooo#.", "...#o#oo##..", "....##o#....",
    "......#.....", "......#.....", "......#.....", "......#.....", ".....###....", "............"};
static const char *const crop0_pic[8] = {
    "........", "........", "........", "...#....", "..#.#...", "...#.#..", "...#.#..", ".#.#.#.#"};
static const char *const crop1_pic[8] = {
    "........", ".#....#.", "#.#..#.#", ".#.#..#.", ".#.#.#..", "..#.#.#.", "..#.#.#.", ".#.#.#.#"};
static const char *const crop2_pic[8] = {
    "#.#.#.#.", "########", "#.#.#.#.", ".#.#.#.#", ".#.#.#.#", "..#.#.#.", "..#.#.#.", ".#.#.#.#"};

/* ---- sprites ------------------------------------------------------------------------------ */

/* Scaled: picture row j covers screen rows [ys[j], ys[j + 1]); pixel columns go in groups that
   share one cast column (8 in moving frames, 4 when still; moving 2, the Full size's moving frames:
   groups of 8 px, two to a cast column), whose texels give a black mask and a
   drawn mask for each picture row; then each row span in front of the runs is filled a byte at a
   time. */
typedef struct SpanCtx {
    short ys[33];
    union { unsigned char b[32]; long l[8]; } bm, wm;
    const unsigned char *dim;
    unsigned char *cb;             /* the group's byte in row 0 */
    short ph, jj;
} SpanCtx;

#ifdef SPRITE_ASM                   /* sprite.s: the same walk in 68000 code; measured no faster */
#include <stddef.h>
_Static_assert(offsetof(SpanCtx, bm) == 66 && offsetof(SpanCtx, wm) == 98 && offsetof(SpanCtx, dim) == 130
               && offsetof(SpanCtx, cb) == 134 && offsetof(SpanCtx, ph) == 138 && offsetof(SpanCtx, jj) == 140, "SpanCtx");
void sprite_walk_asm(SpanCtx *c, const RRun *r0, long start, long dd, long ylo, long lim);
#endif
short sprite_asm = 1;                /* (tests: 0 draws with the C walk) */

/* pictures turned into columns once: 0 clear, 1 white, 3 black, ph bytes a column */
#define NPICS 48
static const char *const *pic_key[NPICS];
static unsigned char *pic_cols[NPICS], pic_pool[12000];
static short npics, pool_used;

static const unsigned char *columns(const char *const *pic, short pw, short ph)
{
    short i, u, j;
    unsigned char *c;
    for (i = 0; i < npics; i++)
        if (pic_key[i] == pic) return pic_cols[i];
    if (npics >= NPICS || (long)pool_used + pw * ph > (long)sizeof pic_pool) return 0;
    c = pic_pool + pool_used;
    pool_used = (short)(pool_used + pw * ph);
    for (u = 0; u < pw; u++)
        for (j = 0; j < ph; j++) {
            char ch = pic[j][u];
            c[u * ph + j] = (unsigned char)(ch == '#' ? 3 : ch == 'o' ? 1 : 0);
        }
    pic_key[npics] = pic; pic_cols[npics] = c;
    return pic_cols[npics++];
}

static void gather(const unsigned char *cp, short ph, unsigned char m, unsigned char *bm, unsigned char *wm)
{
    do {
        unsigned char c = *cp++;
        if (c) {
            *wm |= m;
            if (c & 2) *bm |= m;
        }
        bm++; wm++;
    } while (--ph);
}

/* the picture rows over screen rows [ya, yb) of the group (the runs go down: jj only grows) */
static void spans(SpanCtx *c, short ya, short yb)
{
    short j;
    while (c->jj < c->ph && c->ys[c->jj + 1] <= ya) c->jj++;
    for (j = c->jj; j < c->ph && c->ys[j] < yb; j++) {
        short y0 = c->ys[j] > ya ? c->ys[j] : ya, y1 = c->ys[j + 1] < yb ? c->ys[j + 1] : yb, n = (short)(y1 - y0);
        unsigned char w = c->wm.b[j], k = c->bm.b[j], nw = (unsigned char)~w;
        unsigned char *p = c->cb + 2 * y0;
        if (!w || n <= 0) continue;
        if (c->dim) {
            const unsigned char *dm = c->dim;
            for (; y0 < y1; y0++, p += 2)
                *p = (unsigned char)((*p & nw) | k | (w & dm[y0 & 7]));
        } else if (w == 0xFF) {                                /* whole bytes: 4 rows a turn */
            for (; n >= 4; n -= 4, p += 8) { p[0] = k; p[2] = k; p[4] = k; p[6] = k; }
            for (; n > 0; n--, p += 2) *p = k;
        } else {
            for (; n >= 2; n -= 2, p += 4) {
                p[0] = (unsigned char)((p[0] & nw) | k);
                p[2] = (unsigned char)((p[2] & nw) | k);
            }
            if (n) *p = (unsigned char)((*p & nw) | k);
        }
    }
}

/* the rows of a run list over [ylo, lim) where the terrain is farther than dd, merged into spans */
static void walk(SpanCtx *sc, const RRun *r0, short start, short dd, short ylo, short lim)
{
    const RRun *r = (const RRun *)((const char *)r0 + start);
    short y = 0, vs = -1;
    for (;;) {
        short ye = r->yend;
        if (r->d > dd) { if (vs < 0) vs = y; }
        else if (vs >= 0) {
            if (y > ylo) spans(sc, vs > ylo ? vs : ylo, y);
            vs = -1;
        }
        if (ye >= lim) break;
        y = ye;
        r = (const RRun *)((const char *)r0 + r->next);
    }
    if (vs >= 0) spans(sc, vs > ylo ? vs : ylo, lim);
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))     /* (no memset calls for 32 bytes) */
void sprite_draw(const RCam *cam, const Sprite *s, unsigned char *buf, short fw, short fh, short moving)
{
    long c = bw_sin[(cam->yaw + 256) & 1023], sn = bw_sin[cam->yaw & 1023];
    long rx = s->x - cam->x, rz = s->z - cam->z, d, l, cx, half, x0, x1, ytop, ybot, h0, du, uf, acc, step, um;
    const unsigned char *cols = columns(s->pic, s->pw, s->ph);
    SpanCtx sc;
    short x, xa, xb, g, j, pw = s->pw, ph = s->ph, gw = moving ? 8 : 4, dd, ylo, lim;
    if (ph > 32 || !cols) return;
    d = (rx * c + rz * sn) >> 14;
    if (d < 40 || d > 32000) return;
    l = (rz * c - rx * sn) >> 14;
    cx = fw / 2 + l * 256 / d;
    half = (long)s->sw * 128 / d;
    if (half < 1) half = 1;
    x0 = cx - half; x1 = cx + half;
    h0 = fh / 2 + cam->pitch;
    ytop = h0 - (s->y + s->sh - cam->y) * 256 / d;
    ybot = h0 - (s->y - cam->y) * 256 / d;
    if (x1 <= 0 || x0 >= fw || ybot <= 0 || ytop >= fh || ybot <= ytop) return;
    if (ytop < -2000 || ybot > 4000) return;                  /* (the eye inside it) */
    step = ((ybot - ytop) << 16) / ph;
    for (j = 0, acc = 0; j <= ph; j++, acc += step) sc.ys[j] = (short)(ytop + (acc >> 16));
    sc.ys[ph] = (short)ybot;
    sc.ph = ph;
    sc.dim = s->solid ? 0 : s->dim;
    dd = (short)d;
    ylo = (short)(ytop > 0 ? ytop : 0);
    lim = (short)(ybot < fh ? ybot : fh);
    du = ((long)pw << 16) / (x1 - x0);
    xa = (short)(x0 < 0 ? 0 : x0);
    xb = (short)(x1 > fw ? fw : x1);
    uf = (xa - x0) * du;
    x = xa;
    g = (short)(xa / gw);
    {
        short b0 = moving ? g : g >> 1;
        sc.cb = buf + (long)(b0 >> 1) * 2 * fh + (b0 & 1);
    }
    um = moving ? uf + (long)(g * 8 + 4 - xa) * du : 0;     /* moving: the texel at each byte's middle */
    for (; x < xb; g++) {
        short col = moving ? (short)(moving == 2 ? (g >> 1) << 2 : g << 1) : g, lastu = -1, nl = (short)((ph + 3) >> 2);
        unsigned char cm = 0, any = 0;
        if (g != (short)(xa / gw) && (moving || !(g & 1)))            /* the next byte column */
            sc.cb += ((moving ? g : g >> 1) & 1) ? 1 : 2 * fh - 1;
        for (j = 0; j < nl; j++) sc.bm.l[j] = sc.wm.l[j] = 0;
        if (moving) {                    /* moving frames are 8 pixels wide: one texel column a byte */
            short u = (short)(um >> 16);
            um += du << 3;
            if (u < 0) u = 0;
            if (u >= pw) u = pw - 1;
            if (s->flip) u = (short)(pw - 1 - u);
            {   /* the group's pixels from x to the byte's end or xb */
                short e = (short)((g + 1) * 8 < xb ? (g + 1) * 8 : xb);
                cm = (unsigned char)((0xFF >> (x & 7)) & (0xFF00 >> (e - g * 8)));
                x = e;
            }
            gather(cols + u * ph, ph, cm, sc.bm.b, sc.wm.b);
            cm = 0;
        }
        for (; x < xb && x < (g + 1) * gw; x++, uf += du) {   /* this group's texel columns */
            short u = (short)(uf >> 16);
            if (u >= pw) u = pw - 1;
            if (s->flip) u = (short)(pw - 1 - u);
            if (u != lastu) {
                if (cm) gather(cols + lastu * ph, ph, cm, sc.bm.b, sc.wm.b);
                lastu = u; cm = 0;
            }
            cm |= (unsigned char)(0x80 >> (x & 7));
        }
        if (cm) gather(cols + lastu * ph, ph, cm, sc.bm.b, sc.wm.b);
        for (j = 0; j < nl; j++) if (sc.wm.l[j]) { any = 1; break; }
        if (!any) continue;
        if (s->solid)
            for (j = 0; j < ph; j++) sc.bm.b[j] = sc.wm.b[j];
        sc.jj = 0;
#ifdef SPRITE_ASM
        if (sprite_asm) sprite_walk_asm(&sc, r_runs[col], r_start[col], dd, ylo, lim);
        else
#endif
        walk(&sc, r_runs[col], r_start[col], dd, ylo, lim);
    }
}

void objs_draw(const RCam *cam, unsigned char *buf, short fw, short fh, short moving, short dmax)
{
    short i, ex = (short)(cam->x >> 8), ez = (short)(cam->z >> 8);
    for (i = 0; i < NOBJ; i++) {
        const Obj *o = &objs[i];
        Sprite sp;
        if (!o->type || o->type == O_DOOR) continue;
        sp.flip = 0; sp.solid = 0; sp.dim = 0;
        if (o->x < ex - dmax || o->x > ex + dmax || o->z < ez - dmax || o->z > ez + dmax) continue;
        sp.x = ((long)o->x << 8) + 128; sp.y = (long)o->y << 8; sp.z = ((long)o->z << 8) + 128;
        if (o->type == O_TORCH) {
            sp.sw = 48; sp.sh = 168; sp.pic = torch_pic; sp.pw = 6; sp.ph = 16;
        } else if (o->state < 16) {
            sp.sw = 192; sp.sh = 192; sp.pic = sapling_pic; sp.pw = 12; sp.ph = 12;
        } else {
            short st = (short)(o->state - 16);
            sp.sw = 224; sp.sh = (short)(64 + st * 20);
            sp.pic = st < 3 ? crop0_pic : st < 6 ? crop1_pic : crop2_pic;
            sp.pw = 8; sp.ph = 8;
        }
        sprite_draw(cam, &sp, buf, fw, fh, moving);
    }
}

/*
 * Neural Pinball physics core. See phys.h for units and the no-tunnelling argument.
 *
 * Contact model: a ball touching a surface is pushed out along the contact normal, then the
 * impulse removes its approach speed relative to the surface (moving flippers and the plunger
 * have a surface velocity, omega x r for flippers) and gives back e times it (e = 0 below the
 * material's resting speed, so balls settle instead of buzzing). Friction is Coulomb: the
 * tangential impulse is at most mu times the normal one. Surfaces are infinitely heavy (the
 * flipper coil and the plunger spring are strong enough that the ball doesn't slow them).
 */
#include "phys.h"
#include "sintab4096.h"

#ifdef PHYS_STATS
long pstat[16];      /* 0 steps 1 submoves 2 cell items 3 bbox pass 4 seg 5 circle 6 arc 7 line 8 disc
                        9 flipper bbox pass 10 flipper full 11 respond 12 normalize 13 touch_point */
#define STAT(i) (pstat[i]++)
#else
#define STAT(i) ((void)0)
#endif

/* 16 x 16 -> 32 signed multiply (one MULS on the 68000) */
#define MULS(a, b) ((int32_t)(int16_t)(a) * (int16_t)(b))

/* 32 / 16 -> 16 signed divide, truncating (DIVS.W on the 68000; callers guarantee no overflow) */
#if defined(__m68k__) || defined(__mc68000__)
static inline int16_t DIVS(int32_t a, int16_t b)
{
    int32_t r = a;
    __asm__("divs.w %1,%0" : "+d"(r) : "d"(b) : "cc");
    return (int16_t)r;
}
#else
#define DIVS(a, b) ((int16_t)((int32_t)(a) / (int16_t)(b)))
#endif

#define VMAX     (16L * 65536)        /* speed cap per axis: 16 px/step (about 1925 px/s) */
#define MARGIN6  (2 * 64)             /* grid/bbox safety margin: 2 px */

/* v - v/2^s, rounding the drag towards zero (an arithmetic shift alone would brake negative
   velocities by one extra unit every step); s >= 31 means no drag */
static inline fx drag(fx v, int16_t s)
{
    if (s >= 31)
        return v;
    return v >= 0 ? v - (v >> s) : v + ((-v) >> s);
}

static inline int16_t clamp16(int32_t v)
{
    return v > 32767 ? 32767 : v < -32767 ? -32767 : (int16_t)v;
}

int16_t fsin(uint16_t a)
{
    return SIN4096[a >> 4];
}

int16_t fcos(uint16_t a)
{
    return SIN4096[(uint16_t)(a + 16384) >> 4];
}

#define FSIN(a) (SIN4096[(uint16_t)(a) >> 4])
#define FCOS(a) (SIN4096[(uint16_t)((a) + 16384) >> 4])

int32_t isqrt32(uint32_t v)
{
    uint32_t r = 0, b = 1UL << 30;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else
            r >>= 1;
        b >>= 2;
    }
    return (int32_t)r;
}

/* Unit vector of (dx,dy) (q14) given d2 = dx^2 + dy^2 (q12). The length is taken with as many
   extra fractional bits as still fit in 16 bits and rounded up, so the vector is accurate to
   about 1/30000 and never longer than 1 (a longer normal would make bounces gain energy).
   Returns the length in q6. */
static int32_t normalize(int32_t dx, int32_t dy, uint32_t d2, int16_t *nx, int16_t *ny)
{
    int16_t k = 0;
    STAT(12);
    int32_t l;
    while (k < 4 && d2 < 0x0FFF0000UL) {    /* keeps the root (rounded up) <= 32767 */
        d2 <<= 2;
        k++;
    }
    l = isqrt32(d2);
    if ((uint32_t)l * (uint32_t)l < d2)
        l++;
    if (l == 0) {
        *nx = 0;
        *ny = -16384;
        return 0;
    }
    *nx = DIVS(dx << (14 + k), (int16_t)l);
    *ny = DIVS(dy << (14 + k), (int16_t)l);
    return l >> k;
}

static void push_event(World *w, uint8_t type, int16_t ball, int16_t id, int16_t val)
{
    Event *e;
    if (w->nev >= MAX_EVENTS)
        return;
    e = &w->ev[w->nev++];
    e->type = type;
    e->ball = (uint8_t)ball;
    e->id = id;
    e->val = val;
}

/* ---------------------------------------------------------------------------- setup */

static void unit(int32_t dx, int32_t dy, int16_t *ux, int16_t *uy, int32_t *len)
{
    int32_t l = normalize(dx, dy, (uint32_t)(dx * dx + dy * dy), ux, uy);
    if (l == 0) {
        *ux = 16384;
        *uy = 0;
    }
    *len = l;
}

static void bbox(Shape *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t grow)
{
    s->minx = (q6)((x0 < x1 ? x0 : x1) - grow);
    s->maxx = (q6)((x0 > x1 ? x0 : x1) + grow);
    s->miny = (q6)((y0 < y1 ? y0 : y1) - grow);
    s->maxy = (q6)((y0 > y1 ? y0 : y1) + grow);
}

/* squared distance (q12) from point (x,y) to shape s's line (segment/sensor) */
static int32_t seg_dist2(const Shape *s, int32_t x, int32_t y)
{
    int32_t dx = x - s->ax, dy = y - s->ay, t, qx, qy;
    t = (dx * s->ux + dy * s->uy) >> 14;
    if (t < 0)
        t = 0;
    if (t > s->len)
        t = s->len;
    qx = dx - ((t * s->ux) >> 14);
    qy = dy - ((t * s->uy) >> 14);
    return qx * qx + qy * qy;
}

/* can a ball whose centre is in the cell (cx,cy) touch shape s? (conservative) */
static int16_t shape_near_cell(const Shape *s, int16_t cx, int16_t cy)
{
    int32_t x0 = (int32_t)cx << CELL_SHIFT, y0 = (int32_t)cy << CELL_SHIFT;
    int32_t x1 = x0 + (1 << CELL_SHIFT), y1 = y0 + (1 << CELL_SHIFT);
    int32_t mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
    int32_t reach = 12 * 64 + BALL_R6 + MARGIN6 + (s->rr > BALL_R6 ? s->rr - BALL_R6 : 0);
    if (s->maxx < x0 || s->minx > x1 || s->maxy < y0 || s->miny > y1)
        return 0;
    if (s->kind == S_SEG || s->kind == S_LINE)
        return seg_dist2(s, mx, my) <= reach * reach;
    if (s->kind == S_ARC) {
        int32_t dx = mx - s->ax, dy = my - s->ay;
        int32_t d = isqrt32((uint32_t)(dx * dx + dy * dy));
        int32_t g = d - s->len;
        return g <= reach && g >= -reach;
    }
    return 1;
}

int16_t phys_init(World *w, const ShapeDef *defs, int16_t n, const Material *mats,
                  q6 table_w, q6 table_h)
{
    int16_t i, cx, cy, pass;
    uint16_t total;
    int32_t l;
    char *p = (char *)w;
    uint32_t k;
    for (k = 0; k < sizeof(World); k++)
        p[k] = 0;
    if (n > MAX_SHAPES)
        return -1;
    w->mats = mats;
    w->table_w = table_w;
    w->table_h = table_h;
    w->gw = (int16_t)((table_w + (1 << CELL_SHIFT) - 1) >> CELL_SHIFT);
    w->gh = (int16_t)((table_h + (1 << CELL_SHIFT) - 1) >> CELL_SHIFT);
    if (w->gw > GRID_MAX_W || w->gh > GRID_MAX_H)
        return -1;
    w->gravity = 1359;                 /* 300 px/s^2 at 120.3 steps/s */
    w->fric_shift = 11;
    w->ramp_g_mul = 2;
    w->sink_vmax = 2550;               /* q10: about 2.5 px/step (300 px/s) */
    w->bowl_k = 2400;
    w->bowl_drag = 6;
    w->ramp_x0 = w->ramp_y0 = -32767;
    w->ramp_x1 = w->ramp_y1 = 32767;
    w->gravity2 = w->gravity * w->ramp_g_mul;
    w->drain_y = ((fx)table_h << 10) + (8L << 16);
    w->out_x0 = -(8L << 16);
    w->out_x1 = ((fx)table_w << 10) + (8L << 16);
    w->out_y0 = -(16L << 16);
    w->fbox_x0 = w->fbox_y0 = 32767;
    w->fbox_x1 = w->fbox_y1 = -32767;
    for (i = 0; i < n; i++) {
        const ShapeDef *d = &defs[i];
        Shape *s = &w->shapes[i];
        s->kind = d->kind;
        s->mat = d->mat;
        s->flags = d->flags;
        s->layers = d->layers ? d->layers : 1;
        s->id = d->id;
        s->ax = d->x0;
        s->ay = d->y0;
        switch (d->kind) {
        case S_SEG:
        case S_LINE:
            s->bx = d->x1;
            s->by = d->y1;
            s->rr = (q6)(BALL_R6 + (int16_t)d->rad * 16);
            unit((int32_t)d->x1 - d->x0, (int32_t)d->y1 - d->y0, &s->ux, &s->uy, &l);
            s->len = (q6)l;
            bbox(s, d->x0, d->y0, d->x1, d->y1, s->rr + MARGIN6);
            if (d->flags & SF_KICKER) {     /* kick direction and speed */
                s->ex = (int16_t)(((int32_t)fcos(d->a0) * (int16_t)d->a1) >> 14);
                s->ey = (int16_t)(((int32_t)fsin(d->a0) * (int16_t)d->a1) >> 14);
            }
            break;
        case S_CIRCLE:
        case S_DISC:
            s->len = d->x1;
            if (d->kind == S_DISC && (d->flags & SF_BOWL) && w->nbowls < 4) {
                w->bowl_box[w->nbowls][0] = ((fx)d->x0 - d->x1) << 10;
                w->bowl_box[w->nbowls][1] = ((fx)d->y0 - d->x1) << 10;
                w->bowl_box[w->nbowls][2] = ((fx)d->x0 + d->x1) << 10;
                w->bowl_box[w->nbowls][3] = ((fx)d->y0 + d->x1) << 10;
                w->bowls[w->nbowls++] = i;
            }
            bbox(s, d->x0, d->y0, d->x0, d->y0, (int32_t)d->x1 + BALL_R6 + MARGIN6);
            break;
        case S_ARC:
            s->len = d->x1;
            s->ux = fcos(d->a0);
            s->uy = fsin(d->a0);
            s->ex = fcos(d->a1);
            s->ey = fsin(d->a1);
            bbox(s, d->x0, d->y0, d->x0, d->y0, (int32_t)d->x1 + BALL_R6 + MARGIN6);
            break;
        default:
            return -1;
        }
    }
    w->nshapes = n;
    /* broad phase: a uniform grid of 16 px cells, each listing the shapes a ball centred in
       it could touch. Two passes: count, then fill. */
    for (pass = 0; pass < 2; pass++) {
        total = 0;
        for (cy = 0; cy < w->gh; cy++)
            for (cx = 0; cx < w->gw; cx++) {
                int16_t c = (int16_t)(cy * w->gw + cx);
                if (pass)
                    w->cell_start[c] = total;
                for (i = 0; i < n; i++)
                    if (shape_near_cell(&w->shapes[i], cx, cy)) {
                        if (total >= GRID_ITEMS_MAX)
                            return -1;
                        if (pass)
                            w->cell_items[total] = (uint16_t)(i * (int16_t)sizeof(Shape));
                        total++;
                    }
            }
        if (pass)
            w->cell_start[w->gw * w->gh] = total;
    }
    /* open cells: nothing within reach of the cell or its neighbours, so a ball there can move a
       whole cell (16 px per axis) in one go without missing a contact */
    for (cy = 0; cy < w->gh; cy++)
        for (cx = 0; cx < w->gw; cx++) {
            int16_t ox, oy, open = 1;
            for (oy = -1; oy <= 1; oy++)
                for (ox = -1; ox <= 1; ox++) {
                    int16_t nx = (int16_t)(cx + ox), ny = (int16_t)(cy + oy), c;
                    if (nx < 0 || ny < 0 || nx >= w->gw || ny >= w->gh) {
                        open = 0;
                        continue;
                    }
                    c = (int16_t)(ny * w->gw + nx);
                    if (w->cell_start[c + 1] != w->cell_start[c])
                        open = 0;
                }
            w->cell_open[cy * w->gw + cx] = (uint8_t)open;
        }
    return 0;
}

int16_t phys_add_flipper(World *w, const FlipperDef *d)
{
    Flipper *f;
    int16_t diff;
    int32_t reach;
    if (w->nflip >= MAX_FLIPPERS)
        return -1;
    f = &w->flip[w->nflip];
    f->d = *d;
    diff = (int16_t)(uint16_t)(d->up - d->rest);
    f->dir = diff >= 0 ? 1 : -1;
    f->a_rest = (int32_t)d->rest * 16;
    f->a_up = f->a_rest + (int32_t)diff * 16;
    f->acc_step = (int16_t)(d->rise > 0 ? d->acc / d->rise : d->acc);
    f->ang_from = f->ang_to = f->a_rest;
    /* 2.5 px of tip travel = 2.5 / len radians = 2.5 * 65536 * 16 / (2 pi len) angle units*16 */
    f->sub_th = (int32_t)((5L * 32 * 166886L) / d->len);     /* 2.5 px */
    f->ang = f->a_rest;
    f->w = 0;
    f->ux = fcos(d->rest);
    f->uy = fsin(d->rest);
    /* bounding box of the swept sector: the tip at 33 angles from rest to up, plus the pivot,
       grown by the ball radius, the base radius and a margin */
    reach = d->rb + BALL_R6 + MARGIN6;
    f->minx = f->maxx = d->px;
    f->miny = f->maxy = d->py;
    {
        int16_t k;
        for (k = 0; k <= 32; k++) {
            uint16_t a = (uint16_t)(d->rest + (int16_t)((int32_t)diff * k / 32));
            int16_t tx = (int16_t)(d->px + (MULS(d->len, fcos(a)) >> 14));
            int16_t ty = (int16_t)(d->py + (MULS(d->len, fsin(a)) >> 14));
            if (tx < f->minx) f->minx = tx;
            if (tx > f->maxx) f->maxx = tx;
            if (ty < f->miny) f->miny = ty;
            if (ty > f->maxy) f->maxy = ty;
        }
    }
    f->minx = (q6)(f->minx - reach);
    f->maxx = (q6)(f->maxx + reach);
    f->miny = (q6)(f->miny - reach);
    f->maxy = (q6)(f->maxy + reach);
    reach += d->len;
    f->reach2 = (uint32_t)reach * (uint32_t)reach;
    if (f->minx < w->fbox_x0) w->fbox_x0 = f->minx;
    if (f->miny < w->fbox_y0) w->fbox_y0 = f->miny;
    if (f->maxx > w->fbox_x1) w->fbox_x1 = f->maxx;
    if (f->maxy > w->fbox_y1) w->fbox_y1 = f->maxy;
    return w->nflip++;
}

void phys_set_ramp_box(World *w, q6 x0, q6 y0, q6 x1, q6 y1)
{
    w->ramp_x0 = x0;
    w->ramp_y0 = y0;
    w->ramp_x1 = x1;
    w->ramp_y1 = y1;
}

void phys_set_plunger(World *w, q6 x0, q6 x1, q6 yrest, q6 pull_max, fx pull_rate,
                      int32_t speed_per_px)
{
    Plunger *p = &w->plunger;
    p->x0 = x0;
    p->x1 = x1;
    p->yrest = (fx)yrest << 10;
    p->pull = 0;
    p->vy = 0;
    p->pull_max = (fx)pull_max << 10;
    p->pull_rate = pull_rate;
    p->tip_prev = p->yrest;
    p->speed_per_px = speed_per_px;
    w->has_plunger = 1;
    if (x0 < w->fbox_x0) w->fbox_x0 = x0;
    if (x1 > w->fbox_x1) w->fbox_x1 = x1;
    if ((q6)(yrest - pull_max - BALL_R6) < w->fbox_y0) w->fbox_y0 = (q6)(yrest - pull_max - BALL_R6);
    if ((q6)(yrest + pull_max + BALL_R6) > w->fbox_y1) w->fbox_y1 = (q6)(yrest + pull_max + BALL_R6);
}

/* ---------------------------------------------------------------------------- control */

void phys_flipper_button(World *w, int16_t i, int16_t held)
{
    if (i >= 0 && i < w->nflip)
        w->flip[i].held = (uint8_t)(held != 0);
}

void phys_plunger_button(World *w, int16_t held)
{
    w->plunger.held = (uint8_t)(held != 0);
}

int16_t phys_add_ball(World *w, fx x, fx y, fx vx, fx vy)
{
    int16_t i;
    for (i = 0; i < MAX_BALLS; i++)
        if (w->ball[i].state == BALL_OFF) {
            Ball *b = &w->ball[i];
            b->x = x;
            b->y = y;
            b->vx = vx;
            b->vy = vy;
            b->state = BALL_FREE;
            b->layer = 1;
            b->held_shape = -1;
            b->still = 0;
            b->sleep = 0;
            return i;
        }
    return -1;
}

void phys_eject(World *w, int16_t i, fx vx, fx vy)
{
    Ball *b = &w->ball[i];
    if (b->state != BALL_HELD)
        return;
    b->state = BALL_FREE;
    b->vx = vx;
    b->vy = vy;
    b->still = 0;
    b->sleep = 0;
}

void phys_nudge(World *w, fx dvx, fx dvy)
{
    int16_t i;
    for (i = 0; i < MAX_BALLS; i++)
        if (w->ball[i].state == BALL_FREE) {
            w->ball[i].vx += dvx;
            w->ball[i].vy += dvy;
            w->ball[i].sleep = 0;
        }
}

void phys_shape_on(World *w, int16_t s, int16_t on)
{
    if (s < 0 || s >= w->nshapes)
        return;
    if (on) {
        int16_t i;
        for (i = 0; i < MAX_BALLS; i++)
            w->ball[i].sleep = 0;
        w->shapes[s].flags &= (uint16_t)~SF_OFF;
    } else
        w->shapes[s].flags |= SF_OFF;
}

void phys_arm(World *w, int16_t s, int16_t armed)
{
    if (s < 0 || s >= w->nshapes)
        return;
    if (armed)
        w->shapes[s].flags |= SF_ARMED;
    else
        w->shapes[s].flags &= (uint16_t)~SF_ARMED;
}

int16_t phys_find(const World *w, uint8_t kind, int16_t id)
{
    int16_t i;
    for (i = 0; i < w->nshapes; i++)
        if (w->shapes[i].kind == kind && w->shapes[i].id == id)
            return i;
    return -1;
}

int16_t phys_balls_in_play(const World *w)
{
    int16_t i, n = 0;
    for (i = 0; i < MAX_BALLS; i++)
        if (w->ball[i].state != BALL_OFF)
            n++;
    return n;
}

/* ---------------------------------------------------------------------------- contacts */

/* Resolve a contact: n = unit normal from the surface towards the ball (q14), pen = overlap
   (q6), (wvx,wvy) = surface velocity (q16). Returns the approach speed (q10), 0 if the ball
   was already separating. */
static int16_t balance_push;           /* set by touch_point for a near-vertical point contact */

static int16_t respond(Ball *b, int16_t nx, int16_t ny, q6 pen, fx wvx, fx wvy, const Material *m)
{
    int16_t push = balance_push;
    balance_push = 0;
    int16_t rx, ry, vn, e8, imp, nn;
    int32_t sn, lim, vn2;
    STAT(11);
    b->x += MULS(nx, pen) >> 4;
    b->y += MULS(ny, pen) >> 4;
    rx = clamp16((b->vx - wvx + 32) >> 6);          /* relative velocity, q10 (+-32 px/step) */
    ry = clamp16((b->vy - wvy + 32) >> 6);
    sn = MULS(rx, nx) + MULS(ry, ny);               /* (v . n), q26 */
    if (sn >= 0)
        return 0;
    /* Normal speed = (v . n) / (n . n): exact even though the q14 normal is not exactly unit
       length, so an elastic bounce keeps its energy (rounded to nearest, unbiased). */
    nn = (int16_t)((MULS(nx, nx) + MULS(ny, ny) + 8192) >> 14);
    lim = 32767L * nn;
    vn = sn <= -lim ? (int16_t)-32767 : DIVS(sn - (nn >> 1), nn);
    if (vn >= 0)
        return 0;
    imp = (int16_t)-vn;
    e8 = imp > m->vrest ? m->e8 : 0;
    if (e8) {
        /* the ball overshot the surface by pen during the step; a real one would have bounced
           there and be e * pen back out by now: mirror the overshoot */
        q6 extra = (q6)(MULS(pen, e8) >> 8);
        b->x += MULS(nx, extra) >> 4;
        b->y += MULS(ny, extra) >> 4;
    }
    vn2 = (MULS(vn, 256 + e8) + 128) >> 8;          /* (1 + e) times the approach speed */
    if (vn2 < -32767) {                             /* (only at extreme speeds) */
        b->vx -= (MULS(vn, nx) + 128) >> 8;
        b->vy -= (MULS(vn, ny) + 128) >> 8;
        vn2 -= vn;
    }
    b->vx -= (MULS(vn2, nx) + 128) >> 8;
    b->vy -= (MULS(vn2, ny) + 128) >> 8;
    if (m->mu8 && e8) {     /* Coulomb friction along t = (-ny, nx), in impacts only: a resting
                               ball rolls, it doesn't stick to a slope */
        int16_t vt = (int16_t)((MULS(ry, nx) - MULS(rx, ny) + 8192) >> 14);
        int16_t jmax = (int16_t)((MULS(imp, m->mu8) + 128) >> 8);
        if (vt > jmax)
            vt = jmax;
        else if (vt < -jmax)
            vt = (int16_t)-jmax;
        b->vx += (MULS(vt, ny) + 128) >> 8;
        b->vy -= (MULS(vt, nx) + 128) >> 8;
    }
    if (push)
        b->vx += push * 1024L;           /* 0.016 px/step (2 px/s): off the top of a post */
    return imp;
}

static void kick(Ball *b, int16_t nx, int16_t ny, int16_t speed)
{
    b->vx += (MULS(speed, nx) + 128) >> 8;
    b->vy += (MULS(speed, ny) + 128) >> 8;
}

/* contact with a point/circle at distance (dx,dy) from the ball centre's view: the ball is at
   centre + (dx,dy); rr = touching distance. Fills normal and overlap; 0 if not touching. */
static int16_t touch_point(int16_t dx, int16_t dy, q6 rr, int16_t *nx, int16_t *ny, q6 *pen)
{
    uint32_t d2;
    int32_t dist;
    if (dx >= rr || dx <= -rr || dy >= rr || dy <= -rr)
        return 0;
    d2 = (uint32_t)(MULS(dx, dx) + MULS(dy, dy));
    if (d2 >= (uint32_t)MULS(rr, rr))
        return 0;
    dist = normalize(dx, dy, d2, nx, ny);
    /* a ball exactly on top of a post would balance there forever in exact arithmetic; real
       balls never do: respond() gives it a tiny push sideways */
    if (*ny < 0 && *nx < 64 && *nx > -64)
        balance_push = *nx < 0 ? -1 : 1;
    *pen = (q6)(rr - dist);
    return 1;
}

static void shape_hit(World *w, Shape *s, Ball *b, int16_t bi, int16_t nx, int16_t ny, q6 pen)
{
    const Material *m = &w->mats[s->mat];
    int16_t imp = respond(b, nx, ny, pen, 0, 0, m);
    uint16_t now = (uint16_t)w->steps;
    if (s->flags & SF_BUMPER) {
        if ((int16_t)(now - (uint16_t)s->cool) >= 0) {
            /* a real bumper never kicks exactly along the radius: turn the kick by up to
               +-1.5 degrees, pseudo-randomly but deterministically (from the step count) */
            int16_t r = (int16_t)(((w->steps * 2654435761UL) >> 20) & 0x3FF) - 512, kx, ky;
            r = (int16_t)(r >> 1);
            kx = (int16_t)(nx + (MULS(ny, r) >> 14));
            ky = (int16_t)(ny - (MULS(nx, r) >> 14));
            kick(b, kx, ky, m->kick);
            s->cool = (int16_t)(now + 6);
            push_event(w, EV_BUMPER, bi, s->id, imp);
        }
        return;
    }
    if (!imp)
        return;
    if ((s->flags & SF_KICK) && imp >= m->kmin && (int16_t)(now - (uint16_t)s->cool) >= 0) {
        kick(b, nx, ny, m->kick);
        s->cool = (int16_t)(now + 6);
        push_event(w, EV_SLING, bi, s->id, imp);
        return;
    }
    if (s->flags & SF_DROP) {
        if (imp >= 200) {                         /* about 25 px/s: any real hit */
            s->flags |= SF_OFF;
            push_event(w, EV_DROP, bi, s->id, imp);
        }
        return;
    }
    if (s->flags & SF_EVENT)
        push_event(w, EV_HIT, bi, s->id, imp);
    else if (imp > 2048)
        push_event(w, EV_WALL, bi, s->id, imp);
}

/* line sensor crossing between (qx,qy) and (px,py) */
static void sensor_line(World *w, Shape *s, Ball *b, int16_t bi, q6 qx, q6 qy, q6 px, q6 py)
{
    int32_t d0, d1, t;
    int16_t dx = (int16_t)(px - s->ax), dy = (int16_t)(py - s->ay);
    int16_t ox = (int16_t)(qx - s->ax), oy = (int16_t)(qy - s->ay);
    d1 = MULS(dy, s->ux) - MULS(dx, s->uy);
    d0 = MULS(oy, s->ux) - MULS(ox, s->uy);
    if ((d0 < 0) == (d1 < 0))
        return;
    t = (MULS(dx, s->ux) + MULS(dy, s->uy)) >> 14;
    if (t < 0 || t > s->len)
        return;
    if (s->flags & SF_LAYER) {
        int16_t to = d1 >= 0 ? (s->id & 0xFF) : ((s->id >> 8) & 0xFF);
        if (to)
            b->layer = (uint8_t)to;
        return;
    }
    if (s->flags & SF_KICKER) {
        if (s->flags & SF_ARMED) {
            b->vx = (fx)s->ex << 6;
            b->vy = (fx)s->ey << 6;
            s->flags &= (uint16_t)~SF_ARMED;
            push_event(w, EV_KICKBACK, bi, s->id, 0);
        } else
            push_event(w, EV_SENSOR, bi, s->id, d1 >= 0 ? 1 : -1);
        return;
    }
    if (s->flags & SF_SPINNER) {
        int16_t sp = (int16_t)((MULS(clamp16(b->vx >> 6), -s->uy) + MULS(clamp16(b->vy >> 6), s->ux)) >> 14);
        push_event(w, EV_SPIN, bi, s->id, sp < 0 ? (int16_t)-sp : sp);
        return;
    }
    push_event(w, EV_SENSOR, bi, s->id, d1 >= 0 ? 1 : -1);
}

static void sensor_disc(World *w, Shape *s, Ball *b, int16_t bi, int16_t si, q6 px, q6 py)
{
    int16_t dx = (int16_t)(px - s->ax), dy = (int16_t)(py - s->ay);
    int16_t r = s->len;
    int16_t ax, ay;
    if (!(s->flags & SF_SINK))
        return;
    if (dx >= r || dx <= -r || dy >= r || dy <= -r)
        return;
    if (MULS(dx, dx) + MULS(dy, dy) >= MULS(r, r))
        return;
    ax = clamp16(b->vx >> 6);
    ay = clamp16(b->vy >> 6);
    if (ax < 0) ax = (int16_t)-ax;
    if (ay < 0) ay = (int16_t)-ay;
    if (ax > w->sink_vmax || ay > w->sink_vmax)
        return;
    b->state = BALL_HELD;
    b->held_shape = si;
    b->x = (fx)s->ax << 10;
    b->y = (fx)s->ay << 10;
    b->vx = 0;
    b->vy = 0;
    push_event(w, EV_SINK, bi, s->id, 0);
}

/* collisions of ball b (centre now at px,py; was at qx,qy) with the static shapes near it */
static void collide_static(World *w, Ball *b, int16_t bi, q6 qx, q6 qy, q6 px, q6 py)
{
    int16_t cx = (int16_t)(px >> CELL_SHIFT), cy = (int16_t)(py >> CELL_SHIFT), c;
    uint16_t k, end;
    if (cx < 0) cx = 0;
    if (cx >= w->gw) cx = (int16_t)(w->gw - 1);
    if (cy < 0) cy = 0;
    if (cy >= w->gh) cy = (int16_t)(w->gh - 1);
    c = (int16_t)(cy * w->gw + cx);
    end = w->cell_start[c + 1];
    STAT(1);
    for (k = w->cell_start[c]; k < end; k++) {
        Shape *s = (Shape *)((char *)w->shapes + w->cell_items[k]);   /* items are byte offsets */
        int16_t nx, ny, dx, dy;
        q6 pen;
        STAT(2);
        if ((s->flags & SF_OFF) || !(s->layers & b->layer))
            continue;
        if (px < s->minx || px > s->maxx || py < s->miny || py > s->maxy)
            continue;
        STAT(3);
        STAT(3 + s->kind);
        dx = (int16_t)(px - s->ax);
        dy = (int16_t)(py - s->ay);
        switch (s->kind) {
        case S_SEG: {
            int32_t t = (MULS(dx, s->ux) + MULS(dy, s->uy)) >> 14;
            if (t <= 0) {
                if (!touch_point(dx, dy, s->rr, &nx, &ny, &pen))
                    continue;
            } else if (t >= s->len) {
                if (!touch_point((int16_t)(px - s->bx), (int16_t)(py - s->by), s->rr, &nx, &ny, &pen))
                    continue;
            } else {
                int16_t d = (int16_t)((MULS(dy, s->ux) - MULS(dx, s->uy)) >> 14);
                if (d >= s->rr || d <= -s->rr)
                    continue;
                if (d >= 0) {
                    nx = (int16_t)-s->uy;
                    ny = s->ux;
                    pen = (q6)(s->rr - d);
                } else {
                    nx = s->uy;
                    ny = (int16_t)-s->ux;
                    pen = (q6)(s->rr + d);
                }
            }
            if (s->flags & SF_ONEWAY) {
                /* solid only for a ball on the normal side that is moving against it */
                int16_t d = (int16_t)((MULS(dy, s->ux) - MULS(dx, s->uy)) >> 14);
                int32_t vn = MULS(clamp16(b->vx >> 6), -s->uy) + MULS(clamp16(b->vy >> 6), s->ux);
                if (d < 0 || vn >= 0)
                    continue;
            }
            shape_hit(w, s, b, bi, nx, ny, pen);
            break;
        }
        case S_CIRCLE:
            if (!touch_point(dx, dy, (q6)(BALL_R6 + s->len), &nx, &ny, &pen))
                continue;
            shape_hit(w, s, b, bi, nx, ny, pen);
            break;
        case S_ARC: {
            int32_t d2, dist, rin = s->len - BALL_R6, rout = s->len + BALL_R6;
            if (MULS(s->ux, dy) - MULS(s->uy, dx) < 0 || MULS(dx, s->ey) - MULS(dy, s->ex) < 0) {
                /* outside the arc's angle range: its two end points */
                int16_t ex = (int16_t)(MULS(s->len, s->ux) >> 14), ey = (int16_t)(MULS(s->len, s->uy) >> 14);
                if (touch_point((int16_t)(dx - ex), (int16_t)(dy - ey), BALL_R6, &nx, &ny, &pen)) {
                    shape_hit(w, s, b, bi, nx, ny, pen);
                    break;
                }
                ex = (int16_t)(MULS(s->len, s->ex) >> 14);
                ey = (int16_t)(MULS(s->len, s->ey) >> 14);
                if (touch_point((int16_t)(dx - ex), (int16_t)(dy - ey), BALL_R6, &nx, &ny, &pen))
                    shape_hit(w, s, b, bi, nx, ny, pen);
                break;
            }
            d2 = MULS(dx, dx) + MULS(dy, dy);
            if (d2 <= MULS(rin, rin) || d2 >= MULS(rout, rout))
                continue;
            dist = normalize(dx, dy, (uint32_t)d2, &nx, &ny);
            if (dist == 0)
                continue;
            if (dist < s->len) {          /* inside the circle: pushed towards the centre */
                nx = (int16_t)-nx;
                ny = (int16_t)-ny;
                pen = (q6)(dist - rin);
            } else
                pen = (q6)(rout - dist);
            shape_hit(w, s, b, bi, nx, ny, pen);
            break;
        }
        case S_LINE:
            sensor_line(w, s, b, bi, qx, qy, px, py);
            if (b->state != BALL_FREE)
                return;
            break;
        case S_DISC:
            sensor_disc(w, s, b, bi, (int16_t)(s - w->shapes), px, py);
            if (b->state != BALL_FREE)
                return;
            break;
        }
        px = (q6)(b->x >> 10);
        py = (q6)(b->y >> 10);
    }
}

static void collide_flipper(World *w, Ball *b, int16_t bi, Flipper *f, int16_t fi)
{
    q6 px = (q6)(b->x >> 10), py = (q6)(b->y >> 10);
    int16_t dx, dy, t, r, qx, qy, nx, ny, cxp, cyp;
    q6 pen, rr;
    int16_t imp;
    if (px < f->minx || px > f->maxx || py < f->miny || py > f->maxy || !(f->d.layers & b->layer))
        return;
    STAT(9);
    dx = (int16_t)(px - f->d.px);
    dy = (int16_t)(py - f->d.py);
    if ((uint32_t)(MULS(dx, dx) + MULS(dy, dy)) > f->reach2)
        return;
    STAT(10);
    t = (int16_t)((MULS(dx, f->ux) + MULS(dy, f->uy)) >> 14);
    if (t > 0 && t < f->d.len) {
        /* along the body: the normal is perpendicular to the axis (the 2 degree taper is
           ignored), the distance is the cross product; no square root needed */
        int16_t d = (int16_t)((MULS(dy, f->ux) - MULS(dx, f->uy)) >> 14);
        r = (int16_t)(f->d.rb + DIVS(MULS(f->d.rt - f->d.rb, t), f->d.len));
        rr = (q6)(BALL_R6 + r);
        if (d >= rr || d <= -rr)
            return;
        if (d >= 0) {
            nx = (int16_t)-f->uy;
            ny = f->ux;
            pen = (q6)(rr - d);
        } else {
            nx = f->uy;
            ny = (int16_t)-f->ux;
            pen = (q6)(rr + d);
        }
        qx = (int16_t)(MULS(t, f->ux) >> 14);
        qy = (int16_t)(MULS(t, f->uy) >> 14);
    } else {
        /* the round ends: the pivot (base radius) or the tip (tip radius) */
        if (t <= 0) {
            t = 0;
            r = f->d.rb;
        } else {
            t = f->d.len;
            r = f->d.rt;
        }
        qx = (int16_t)(MULS(t, f->ux) >> 14);
        qy = (int16_t)(MULS(t, f->uy) >> 14);
        rr = (q6)(BALL_R6 + r);
        if (!touch_point((int16_t)(dx - qx), (int16_t)(dy - qy), rr, &nx, &ny, &pen))
            return;
    }
    /* the flipper's surface velocity at the contact point c = q + n r: omega x c */
    cxp = (int16_t)(qx + (MULS(nx, r) >> 14));
    cyp = (int16_t)(qy + (MULS(ny, r) >> 14));
    imp = respond(b, nx, ny, pen, -(MULS(f->wq14, cyp) >> 4), MULS(f->wq14, cxp) >> 4,
                  &w->mats[f->d.mat]);
    if (imp > 600)
        push_event(w, EV_FLIPHIT, bi, fi, imp);
}

static void collide_plunger(World *w, Ball *b)
{
    Plunger *p = &w->plunger;
    q6 px = (q6)(b->x >> 10);
    fx tip = p->yrest + p->pull, bottom = b->y + ((fx)BALL_R << 16);
    /* swept: a ball whose bottom is below the tip but was above where the tip was at the start
       of the step (a fast plunger stroke moves up to 14 px per step) */
    if (px < p->x0 || px > p->x1 || bottom <= tip || bottom > p->tip_prev + (2L << 16))
        return;
    static const Material dead = {0, 0, 0, 0, 0};
    respond(b, 0, -16384, (q6)((bottom - tip) >> 10), 0, p->vy, &dead);
    if (p->vy < 0 && b->vy > p->vy)
        b->vy = p->vy;                         /* carried by the plunger stroke */
}

static void collide_balls(World *w)
{
    int16_t i, j, nfree = 0;
    for (i = 0; i < MAX_BALLS; i++)
        if (w->ball[i].state == BALL_FREE)
            nfree++;
    if (nfree < 2)
        return;
    for (i = 0; i < MAX_BALLS; i++) {
        Ball *a = &w->ball[i];
        if (a->state != BALL_FREE)
            continue;
        for (j = (int16_t)(i + 1); j < MAX_BALLS; j++) {
            Ball *b = &w->ball[j];
            int32_t ddx, ddy;
            int16_t nx, ny, vn, rx, ry;
            q6 pen;
            if (b->state != BALL_FREE || b->layer != a->layer)
                continue;
            ddx = (b->x - a->x) >> 10;
            ddy = (b->y - a->y) >> 10;
            if (ddx >= 2 * BALL_R6 || ddx <= -2 * BALL_R6 || ddy >= 2 * BALL_R6 || ddy <= -2 * BALL_R6)
                continue;
            if (!touch_point((int16_t)ddx, (int16_t)ddy, 2 * BALL_R6, &nx, &ny, &pen))
                continue;
            balance_push = 0;
            /* separate equally; exchange the normal velocity (equal masses, e = 0.9) */
            a->x -= MULS(nx, pen) >> 5;
            a->y -= MULS(ny, pen) >> 5;
            b->x += MULS(nx, pen) >> 5;
            b->y += MULS(ny, pen) >> 5;
            rx = clamp16((b->vx - a->vx) >> 6);
            ry = clamp16((b->vy - a->vy) >> 6);
            vn = (int16_t)((MULS(rx, nx) + MULS(ry, ny)) >> 14);
            if (vn >= 0)
                continue;
            vn = (int16_t)(MULS(vn, 243) >> 8);       /* (1 + 0.9) / 2 */
            a->vx += MULS(vn, nx) >> 8;
            a->vy += MULS(vn, ny) >> 8;
            b->vx -= MULS(vn, nx) >> 8;
            b->vy -= MULS(vn, ny) >> 8;
            if (vn < -1200)
                push_event(w, EV_BALLHIT, i, j, (int16_t)-vn);
        }
    }
}

/* ---------------------------------------------------------------------------- stepping */

/* angular dynamics for one step; the angle itself moves in the sub-steps of phys_step */
static void update_flipper(World *w, Flipper *f)
{
    int16_t held = f->held && !w->tilted;
    int32_t a = f->ang;
    int32_t w16 = f->w;
    f->ang_from = a;
    if (!w16 && a == (held ? f->a_up : f->a_rest)) {     /* resting on a stop: nothing moves */
        if (!held)
            f->on_steps = 0;
        else if (f->on_steps < 32767)
            f->on_steps++;
        f->ang_to = a;
        f->moving = 0;
        f->wq14 = 0;
        return;
    }
    if (held) {
        if (f->on_steps < 32767)
            f->on_steps++;
        if (a == f->a_up)
            w16 = 0;
        else {
            /* the coil's pull builds up over the first `rise` steps of a press */
            int32_t acc = f->d.acc;
            if (f->on_steps < f->d.rise)
                acc = MULS(f->acc_step, f->on_steps);
            if (f->dir > 0)
                w16 += acc;
            else
                w16 -= acc;
            if (w16 > f->d.wmax) w16 = f->d.wmax;
            if (w16 < -f->d.wmax) w16 = -f->d.wmax;
            a += w16;
            if ((f->dir > 0 && a >= f->a_up) || (f->dir < 0 && a <= f->a_up)) {
                a = f->a_up;
                w16 = 0;
            }
        }
    } else {
        f->on_steps = 0;
        if (a == f->a_rest)
            w16 = 0;
        else {
            /* released: a still-rising flipper is braked hard (so a short tap moves it only part
               of the way), then the return spring pulls it down */
            if (f->dir > 0)
                w16 -= w16 > 0 ? f->d.brake : f->d.dec;
            else
                w16 += w16 < 0 ? f->d.brake : f->d.dec;
            if (w16 > f->d.wmax) w16 = f->d.wmax;
            if (w16 < -f->d.wmax) w16 = -f->d.wmax;
            a += w16;
            if ((f->dir > 0 && a <= f->a_rest) || (f->dir < 0 && a >= f->a_rest)) {
                a = f->a_rest;
                w16 = 0;
            }
        }
    }
    f->ang_to = a;
    f->w = w16;
    f->moving = (uint8_t)(w16 != 0);
    /* angle units*16 per step -> radians per step * 16384: * 2 pi / 64 = 0.09817 */
    f->wq14 = (int16_t)((w16 * 201L) >> 11);
}

static void flipper_set_angle(Flipper *f, int32_t a)
{
    if (a != f->ang) {
        f->ang = a;
        f->ux = FCOS(a >> 4);
        f->uy = FSIN(a >> 4);
    }
}

static void update_plunger(Plunger *p)
{
    p->tip_prev = p->yrest + p->pull;
    if (!p->held && !p->firing && p->pull <= 0) {
        p->vy = 0;
        return;
    }
    if (p->held && !p->firing) {
        p->pull += p->pull_rate;
        p->vy = p->pull_rate;
        if (p->pull >= p->pull_max) {
            p->pull = p->pull_max;
            p->vy = 0;
        }
        return;
    }
    if (!p->held && !p->firing && p->pull > 0) {   /* released: the spring drives the tip up */
        p->firing = 1;
        p->vy = -(((p->pull >> 8) * p->speed_per_px) >> 8);
    }
    if (p->firing) {
        p->pull += p->vy;
        if (p->pull <= 0) {
            p->pull = 0;
            p->vy = 0;
            p->firing = 0;
        }
    } else
        p->vy = 0;
}

void phys_step(World *w)
{
    int16_t i, f, k, n, shift = 0, wake = 0, nfree = 0;
    int16_t bshift[MAX_BALLS], act[MAX_BALLS], a;
    int32_t fstep[MAX_FLIPPERS];
    STAT(0);
    for (f = 0; f < w->nflip; f++) {
        update_flipper(w, &w->flip[f]);
        if (w->flip[f].ang_to != w->flip[f].ang_from)
            wake = 1;
    }
    if (w->has_plunger) {
        update_plunger(&w->plunger);
        if (w->plunger.vy || w->plunger.held)
            wake = 1;
    }
    /* forces, and how finely this step must be cut */
    for (i = 0; i < MAX_BALLS; i++) {
        Ball *b = &w->ball[i];
        fx ax;
        if (b->state != BALL_FREE)
            continue;
        if (b->sleep) {
            if (!wake && !b->vx && !b->vy) {
                if (b->still < 65535)
                    b->still++;
                continue;
            }
            b->sleep = 0;
        }
        nfree++;
        b->vy += b->layer == 2 ? w->gravity2 : w->gravity;
        b->vx = drag(b->vx, w->fric_shift);
        b->vy = drag(b->vy, w->fric_shift);
        /* bowls: a parabolic dish pulls the ball towards its centre (gradient descent) */
        for (k = 0; k < w->nbowls; k++) {
            Shape *s = &w->shapes[w->bowls[k]];
            if (b->y > w->bowl_box[k][1] && b->y < w->bowl_box[k][3] && b->x > w->bowl_box[k][0] &&
                b->x < w->bowl_box[k][2] && (s->layers & b->layer)) {
                int16_t dx = (int16_t)((b->x >> 10) - s->ax), dy = (int16_t)((b->y >> 10) - s->ay);
                if (dx < s->len && dx > -s->len && dy < s->len && dy > -s->len &&
                    MULS(dx, dx) + MULS(dy, dy) < MULS(s->len, s->len)) {
                    b->vx -= MULS(dx, w->bowl_k) >> 8;
                    b->vy -= MULS(dy, w->bowl_k) >> 8;
                    b->vx = drag(b->vx, w->bowl_drag);
                    b->vy = drag(b->vy, w->bowl_drag);
                }
            }
        }
        {
            fx vx = b->vx, vy = b->vy, avx = vx < 0 ? -vx : vx, avy = vy < 0 ? -vy : vy;
            if (avx > VMAX) {
                b->vx = vx < 0 ? -VMAX : VMAX;
                avx = VMAX;
            }
            if (avy > VMAX) {
                b->vy = vy < 0 ? -VMAX : VMAX;
                avy = VMAX;
            }
            ax = avx + avy;
        }
        /* sub-moves of at most 3.5 px (|dx| + |dy|, so also in straight-line distance) ... */
        k = ax < 229376L ? 0 : ax < 458752L ? 1 : ax < 917504L ? 2 : ax < 1835008L ? 3 : 4;
        if (k) {
            /* ... except in open table, away from the flippers and the plunger, where one move
               can't reach anything (other balls are checked below) */
            q6 qx = (q6)(b->x >> 10), qy = (q6)(b->y >> 10);
            int16_t cx = (int16_t)(qx >> CELL_SHIFT), cy = (int16_t)(qy >> CELL_SHIFT);
            if (cx >= 0 && cy >= 0 && cx < w->gw && cy < w->gh && w->cell_open[cy * w->gw + cx] &&
                (qx < w->fbox_x0 - 1024 || qx > w->fbox_x1 + 1024 || qy < w->fbox_y0 - 1024 || qy > w->fbox_y1 + 1024))
                k = -k;                          /* (negative: open; made 0 if no ball is near) */
        }
        bshift[i] = k;
        act[nfree - 1] = i;
        if (k > shift)
            shift = k;
    }
    /* open-table balls move in one go unless another ball is within 40 px of them */
    for (a = 0; a < nfree; a++) {
        int16_t j;
        i = act[a];
        if (bshift[i] >= 0)
            continue;
        for (j = 0; j < nfree; j++) {
            const Ball *o = &w->ball[act[j]], *b = &w->ball[i];
            fx ddx, ddy;
            if (act[j] == i)
                continue;
            ddx = o->x - b->x;
            ddy = o->y - b->y;
            if (ddx < (40L << 16) && ddx > -(40L << 16) && ddy < (40L << 16) && ddy > -(40L << 16))
                break;
        }
        if (j < nfree) {
            bshift[i] = (int16_t)-bshift[i];
            if (bshift[i] > shift)
                shift = bshift[i];
        } else
            bshift[i] = 0;
    }
    if (!nfree) {
        for (f = 0; f < w->nflip; f++)
            flipper_set_angle(&w->flip[f], w->flip[f].ang_to);
        w->steps++;
        return;
    }
    /* a fast flipper moves in sub-steps of at most 2.5 px at the tip, and the balls near it
       move in the same sub-steps; balls elsewhere only as finely as their own speed needs */
    for (f = 0; f < w->nflip; f++) {
        Flipper *fl = &w->flip[f];
        int32_t d = fl->ang_to - fl->ang_from;
        if (d < 0)
            d = -d;
        if (d > fl->sub_th) {
            int16_t fs = d <= 2 * fl->sub_th ? 1 : d <= 4 * fl->sub_th ? 2 : d <= 8 * fl->sub_th ? 3 : 4;
            int16_t u0x = FCOS(fl->ang_from >> 4), u0y = FSIN(fl->ang_from >> 4);
            int16_t u1x = FCOS(fl->ang_to >> 4), u1y = FSIN(fl->ang_to >> 4);
            q6 clear = (q6)(fl->d.rb + BALL_R6 + MARGIN6);
            for (a = 0; a < nfree; a++) {
                const Ball *b = &w->ball[i = act[a]];
                q6 qx, qy;
                int16_t dx, dy, s0, s1;
                qx = (q6)(b->x >> 10);
                qy = (q6)(b->y >> 10);
                if (fs <= bshift[i] || qx < fl->minx || qx > fl->maxx || qy < fl->miny || qy > fl->maxy)
                    continue;
                /* the flipper can only hit the ball if it sweeps across it this step: on the
                   same side of the axis at both angles, and clear of it, means no */
                dx = (int16_t)(qx - fl->d.px);
                dy = (int16_t)(qy - fl->d.py);
                s0 = (int16_t)((MULS(dy, u0x) - MULS(dx, u0y)) >> 14);
                s1 = (int16_t)((MULS(dy, u1x) - MULS(dx, u1y)) >> 14);
                if ((s0 > clear && s1 > clear) || (s0 < -clear && s1 < -clear))
                    continue;
                bshift[i] = fs;
                if (fs > shift)
                    shift = fs;
            }
        }
    }
    n = (int16_t)(1 << shift);
    for (f = 0; f < w->nflip; f++)
        fstep[f] = (w->flip[f].ang_to - w->flip[f].ang_from) >> shift;
    for (k = 1; k <= n; k++) {
        for (f = 0; f < w->nflip; f++) {
            Flipper *fl = &w->flip[f];
            if (fl->ang_to != fl->ang_from)          /* (fl->ang == ang_from when k == 1) */
                flipper_set_angle(fl, k == n ? fl->ang_to : fl->ang + fstep[f]);
        }
        for (a = 0; a < nfree; a++) {
            Ball *b = &w->ball[i = act[a]];
            q6 qx, qy;
            int16_t bs;
            if (b->state != BALL_FREE)
                continue;
            bs = bshift[i];
            if (k & ((1 << (shift - bs)) - 1))
                continue;                        /* this ball moves only every 2^(shift - bs) sub-steps */
            qx = (q6)(b->x >> 10);
            qy = (q6)(b->y >> 10);
            b->x += b->vx >> bs;
            b->y += b->vy >> bs;
            collide_static(w, b, i, qx, qy, (q6)(b->x >> 10), (q6)(b->y >> 10));
            if (b->state != BALL_FREE)
                continue;
            qx = (q6)(b->x >> 10);
            qy = (q6)(b->y >> 10);
            if (qx < w->fbox_x0 || qx > w->fbox_x1 || qy < w->fbox_y0 || qy > w->fbox_y1)
                continue;                        /* far from the flippers and the plunger */
            for (f = 0; f < w->nflip; f++) {
                Flipper *fl = &w->flip[f];
                if (qx >= fl->minx && qx <= fl->maxx && qy >= fl->miny && qy <= fl->maxy)
                    collide_flipper(w, b, i, fl, f);
            }
            if (w->has_plunger && qx >= w->plunger.x0 && qx <= w->plunger.x1)
                collide_plunger(w, b);
        }
        if (nfree > 1)
            collide_balls(w);
    }
    for (a = 0; a < nfree; a++) {
        Ball *b = &w->ball[i = act[a]];
        fx ax;
        if (b->state != BALL_FREE)
            continue;
        if (b->layer == 2) {           /* safety net: a ramp ball outside the ramp is on the playfield */
            q6 qx = (q6)(b->x >> 10), qy = (q6)(b->y >> 10);
            if (qx < w->ramp_x0 || qx > w->ramp_x1 || qy < w->ramp_y0 || qy > w->ramp_y1)
                b->layer = 1;
        }
        if (b->y > w->drain_y) {
            b->state = BALL_OFF;
            push_event(w, EV_DRAIN, i, 0, 0);
            continue;
        }
        if (b->x < w->out_x0 || b->x > w->out_x1 || b->y < w->out_y0) {
            b->state = BALL_OFF;
            push_event(w, EV_ESCAPE, i, 0, 0);
            continue;
        }
        ax = (b->vx < 0 ? -b->vx : b->vx) + (b->vy < 0 ? -b->vy : b->vy);
        if (ax < 1600) {
            if (b->still < 65535)
                b->still++;
            if (b->still >= 48 && !wake) {      /* at rest for 0.4 s: sleep */
                b->sleep = 1;
                b->vx = b->vy = 0;
            }
        } else
            b->still = 0;
    }
    w->steps++;
}

uint32_t phys_hash(const World *w)
{
    uint32_t h = 2166136261UL;
    int16_t i;
#define MIX(v) (h = (h ^ (uint32_t)(v)) * 16777619UL)
    for (i = 0; i < MAX_BALLS; i++) {
        const Ball *b = &w->ball[i];
        MIX(b->x); MIX(b->y); MIX(b->vx); MIX(b->vy); MIX(b->state); MIX(b->layer);
    }
    for (i = 0; i < w->nflip; i++) {
        MIX(w->flip[i].ang);
        MIX(w->flip[i].w);
    }
    MIX(w->plunger.pull);
    MIX(w->steps);
    for (i = 0; i < w->nshapes; i++)
        MIX(w->shapes[i].flags);
#undef MIX
    return h;
}

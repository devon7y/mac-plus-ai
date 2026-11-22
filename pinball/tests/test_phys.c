/*
 * M0 host tests for the physics core (pinball/core/phys.c).
 *
 *   cc -O2 -I../core test_phys.c ../core/phys.c -o test_phys && ./test_phys [-v] [--dump NAME]
 *
 * Each test prints PASS/FAIL with its key numbers. --dump NAME writes the ball path of one
 * scenario (x y per step) to stdout for tools/plot_path.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "phys.h"

static int verbose, fails;
static const char *dump_name;
static FILE *dump_f;

#define DEG(a) ((uint16_t)((int32_t)((a) * 65536.0 / 360.0) & 0xFFFF))
#define P6(v) ((q6)lround((v) * 64.0))

enum { M_WALL, M_RUBBER, M_FLIP, M_SLING, M_BUMPER, M_PERFECT, M_DROP, M_NMATS };
static const Material mats[M_NMATS] = {     /* per step at 120.3 steps/s (q10 px/step) */
    /* e8   vrest  mu8  kick   kmin */
    {  115,   400,  20,     0,    0 },     /* wall: e 0.45 */
    {  190,   400,  30,     0,    0 },     /* rubber post: e 0.75 */
    {  140,   500,   5,     0,    0 },     /* flipper rubber: e 0.55, low friction (rolling ball) */
    {  170,   400,  30,  7000, 1400 },     /* slingshot: kicks 820 px/s */
    {  150,   400,  20,  9000,    0 },     /* pop bumper: kicks 1060 px/s */
    {  256,     0,   0,     0,    0 },     /* perfect (e = 1, frictionless): energy tests */
    {   77,   300,  30,     0,    0 },     /* drop target */
};

static ShapeDef defs[MAX_SHAPES];
static int ndefs;

static void seg(double x0, double y0, double x1, double y1, int mat, int flags, int id)
{
    ShapeDef *d = &defs[ndefs++];
    memset(d, 0, sizeof *d);
    d->kind = S_SEG; d->mat = (uint8_t)mat; d->flags = (uint16_t)flags; d->layers = 1; d->id = (int16_t)id;
    d->x0 = P6(x0); d->y0 = P6(y0); d->x1 = P6(x1); d->y1 = P6(y1);
}

static void circle(double x, double y, double r, int mat, int flags, int id)
{
    ShapeDef *d = &defs[ndefs++];
    memset(d, 0, sizeof *d);
    d->kind = S_CIRCLE; d->mat = (uint8_t)mat; d->flags = (uint16_t)flags; d->layers = 1; d->id = (int16_t)id;
    d->x0 = P6(x); d->y0 = P6(y); d->x1 = P6(r);
}

static void arc(double x, double y, double r, double a0, double a1, int mat)
{
    ShapeDef *d = &defs[ndefs++];
    memset(d, 0, sizeof *d);
    d->kind = S_ARC; d->mat = (uint8_t)mat; d->layers = 1;
    d->x0 = P6(x); d->y0 = P6(y); d->x1 = P6(r); d->a0 = DEG(a0); d->a1 = DEG(a1);
}

static World W;

static void world(double tw, double th)
{
    if (phys_init(&W, defs, (int16_t)ndefs, mats, P6(tw), P6(th)) != 0) {
        printf("phys_init failed\n");
        exit(2);
    }
}

static double px(fx v) { return v / 65536.0; }
#define RATE (60.15 * STEPS_PER_TICK)              /* physics steps per second */
#define SEC(t) ((int)((t) * RATE + 0.5))           /* seconds -> steps */
#define VPS(v) ((fx)((v) / RATE * 65536.0))        /* px/s -> fx px/step */
#define TOPV 1900.0                                /* near the speed cap, px/s */
static double spd(const Ball *b) { return sqrt(px(b->vx) * px(b->vx) + px(b->vy) * px(b->vy)); }
static void check(int ok, const char *name, const char *fmt, ...);

#include <stdarg.h>
static void check(int ok, const char *name, const char *fmt, ...)
{
    va_list ap;
    printf("%s %-28s ", ok ? "PASS" : "FAIL", name);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    if (!ok)
        fails++;
}

static void step(int n)
{
    while (n-- > 0) {
        W.nev = 0;                     /* events of the last step stay readable */
        phys_step(&W);
        if (dump_f) {
            int i;
            for (i = 0; i < MAX_BALLS; i++)
                if (W.ball[i].state != BALL_OFF)
                    fprintf(dump_f, "%d %.3f %.3f ", i, px(W.ball[i].x), px(W.ball[i].y));
            for (i = 0; i < W.nflip; i++)
                fprintf(dump_f, "F%d %d ", i, (int)(W.flip[i].ang >> 4));
            fprintf(dump_f, "\n");
        }
    }
}

static void dump_begin(const char *name)
{
    int i;
    dump_f = dump_name && !strcmp(dump_name, name) ? stdout : NULL;
    if (!dump_f)
        return;
    for (i = 0; i < W.nshapes; i++) {
        const Shape *sh = &W.shapes[i];
        double a0 = atan2(sh->uy, sh->ux) * 180 / 3.14159265, a1 = atan2(sh->ey, sh->ex) * 180 / 3.14159265;
        fprintf(dump_f, "S %d %.2f %.2f %.2f %.2f %.2f %.1f %.1f\n", sh->kind, sh->ax / 64.0, sh->ay / 64.0,
                sh->bx / 64.0, sh->by / 64.0, sh->len / 64.0, a0, a1);
    }
    for (i = 0; i < W.nflip; i++)
        fprintf(dump_f, "FD %d %.2f %.2f %.2f\n", i, W.flip[i].d.px / 64.0, W.flip[i].d.py / 64.0, W.flip[i].d.len / 64.0);
}
static void dump_end(void) { dump_f = NULL; }

static uint32_t rng = 12345;
static double frand(void)
{
    rng = rng * 1103515245u + 12345u;
    return ((rng >> 8) & 0xFFFFFF) / 16777216.0;
}

/* ---------------------------------------------------------------- energy */

static void test_energy_box(void)
{
    /* frictionless, perfectly elastic box, no gravity: kinetic energy must stay constant */
    int i, trial;
    double worst = 0;
    ndefs = 0;
    seg(20, 20, 220, 20, M_PERFECT, 0, 0);
    seg(220, 20, 220, 300, M_PERFECT, 0, 0);
    seg(220, 300, 20, 300, M_PERFECT, 0, 0);
    seg(20, 300, 20, 20, M_PERFECT, 0, 0);
    circle(120, 160, 15, M_PERFECT, 0, 0);
    arc(120, 160, 60, 200, 340, M_PERFECT);
    for (trial = 0; trial < 20; trial++) {
        double e0, e1, a = frand() * 6.283, v = (150 + 1700 * frand()) / RATE;
        world(240, 320);
        W.gravity = 0;
        W.fric_shift = 31;
        phys_add_ball(&W, FXPX(40) + (fx)(frand() * 65536 * 20), FXPX(250), (fx)(v * cos(a) * 65536), (fx)(v * sin(a) * 65536));
        e0 = spd(&W.ball[0]);
        for (i = 0; i < 48000; i++) {   /* 100 s */
            phys_step(&W);
            W.nev = 0;
        }
        e1 = spd(&W.ball[0]);
        if (fabs(e1 * e1 / (e0 * e0) - 1) > worst)
            worst = fabs(e1 * e1 / (e0 * e0) - 1);
        if (verbose)
            printf("   trial %d: speed %.4f -> %.4f px/step\n", trial, e0, e1);
        if (W.ball[0].state != BALL_FREE)
            worst = 99;
    }
    check(worst < 0.02, "energy: elastic box", "worst kinetic energy change over 100 s: %.2f%% (20 random shots)", worst * 100);
}

static void test_energy_gravity(void)
{
    /* gravity, perfectly elastic floor, no friction: bounce apex height stays put */
    int i, bounces = 0;
    double apex0 = 0, apex = 0, top = 1e9, prev_vy = 0;
    ndefs = 0;
    seg(0, 300, 240, 300, M_PERFECT, 0, 0);
    world(240, 320);
    W.fric_shift = 31;
    phys_add_ball(&W, FXPX(120), FXPX(100), 0, 0);
    for (i = 0; i < SEC(60); i++) {
        double vy;
        phys_step(&W);
        W.nev = 0;
        vy = px(W.ball[0].vy);
        if (px(W.ball[0].y) < top)
            top = px(W.ball[0].y);
        if (prev_vy < 0 && vy >= 0) {       /* apex */
            if (!bounces)
                apex0 = top;
            apex = top;
            bounces++;
            top = 1e9;
        }
        prev_vy = vy;
    }
    /* no gain, and a loss of at most 0.4% of the drop per bounce (the contact push-out) */
    check(bounces > 10 && apex >= apex0 - 0.01 && apex - apex0 < 0.004 * bounces * (300 - 4 - 100), "energy: elastic bounce + g",
          "%d bounces in 60 s, first apex y=%.2f, last y=%.2f (start 100)", bounces, apex0, apex);
}

static void test_energy_damped(void)
{
    /* real materials: every bounce must lose energy, and the ball must come to rest */
    int i, bounces = 0, rising = 0;
    double prev_apex = -1, top = 1e9, prev_vy = 0;
    ndefs = 0;
    seg(0, 300, 240, 300, M_RUBBER, 0, 0);
    world(240, 320);
    phys_add_ball(&W, FXPX(120), FXPX(60), 0, 0);
    for (i = 0; i < SEC(20); i++) {
        double vy;
        phys_step(&W);
        W.nev = 0;
        vy = px(W.ball[0].vy);
        if (px(W.ball[0].y) < top)
            top = px(W.ball[0].y);
        if (prev_vy < 0 && vy >= 0) {
            if (prev_apex >= 0 && top < prev_apex - 0.01)
                rising++;
            prev_apex = top;
            bounces++;
            top = 1e9;
        }
        prev_vy = vy;
    }
    check(rising == 0 && bounces > 2 && spd(&W.ball[0]) * RATE < 5 && px(W.ball[0].y) > 295.5,
          "energy: rubber bounces decay", "%d bounces, %d gained height, rest at y=%.2f speed %.4f",
          bounces, rising, px(W.ball[0].y), spd(&W.ball[0]));
}

/* ---------------------------------------------------------------- tunnelling */

static void test_tunnel_walls(void)
{
    /* Shoot balls at the top speed into a thin wall from 360 directions and many offsets.
       The centre must never reach the far side of the wall line. */
    int a, o, crossed = 0, shots = 0;
    ndefs = 0;
    seg(40, 160, 200, 160, M_WALL, 0, 0);
    world(240, 320);
    for (a = 0; a < 360; a += 3)
        for (o = -60; o <= 60; o += 15) {
            double ang = a * 3.14159265 / 180, sx, sy;
            int i, side;
            if (sin(ang) == 0)
                continue;
            side = sin(ang) > 0 ? -1 : 1;           /* start above if moving down */
            sx = 120 + o - cos(ang) * 30;
            sy = 160 + side * 30;
            if (fabs(sin(ang)) < 0.05)
                continue;
            world(240, 320);
            W.gravity = 0;
            phys_add_ball(&W, (fx)(sx * 65536), (fx)(sy * 65536), (fx)(cos(ang) * VPS(TOPV)), (fx)(-side * fabs(sin(ang)) * VPS(TOPV)));
            shots++;
            for (i = 0; i < SEC(0.125); i++) {
                double y;
                phys_step(&W);
                W.nev = 0;
                y = px(W.ball[0].y);
                if ((side < 0 && y > 160) || (side > 0 && y < 160)) {
                    if (px(W.ball[0].x) > 36 && px(W.ball[0].x) < 204) {
                        crossed++;
                        break;
                    }
                }
            }
        }
    check(crossed == 0, "no tunnelling: thin wall", "%d of %d top-speed shots crossed", crossed, shots);
}

static int inside_box(double x, double y) { return x > 20 && x < 220 && y > 20 && y < 300; }

static void test_tunnel_box(void)
{
    /* A closed box with posts and arcs; one ball at top speed bouncing for 5 minutes with no
       energy loss (perfect walls), plus random kicks to keep it at the speed cap. */
    int i, out = 0;
    ndefs = 0;
    seg(20, 20, 220, 20, M_PERFECT, 0, 0);
    seg(220, 20, 220, 300, M_PERFECT, 0, 0);
    seg(220, 300, 20, 300, M_PERFECT, 0, 0);
    seg(20, 300, 20, 20, M_PERFECT, 0, 0);
    seg(60, 200, 180, 230, M_PERFECT, 0, 0);
    seg(70, 90, 75, 140, M_PERFECT, 0, 0);
    circle(150, 100, 10, M_PERFECT, 0, 0);
    circle(100, 250, 3, M_PERFECT, 0, 0);
    arc(120, 120, 50, 0, 120, M_PERFECT);
    world(240, 320);
    W.gravity = 0;
    W.fric_shift = 31;
    phys_add_ball(&W, FXPX(40), FXPX(40), VPS(TOPV), VPS(TOPV * 0.8));
    for (i = 0; i < SEC(300); i++) {
        double x, y;
        if (i % SEC(1) == 0) {  /* re-aim at full speed */
            double a = frand() * 6.283;
            W.ball[0].vx = (fx)(cos(a) * VPS(TOPV));
            W.ball[0].vy = (fx)(sin(a) * VPS(TOPV));
        }
        phys_step(&W);
        W.nev = 0;
        x = px(W.ball[0].x);
        y = px(W.ball[0].y);
        if (!inside_box(x, y) || W.ball[0].state != BALL_FREE) {
            out++;
            break;
        }
    }
    check(out == 0, "no tunnelling: closed box", "5 min at the speed cap (1900 px/s): %s",
          out ? "ESCAPED" : "stayed inside");
}

/* ---------------------------------------------------------------- flippers */

/* the lower playfield used by the flipper tests: two flippers, inlane guides, outer walls */
#define LPX 77.0
#define RPX 165.0
#define PY 282.0
static int lflip, rflip;

static FlipperDef flipdef(double x, double y, int left)
{
    FlipperDef f;
    memset(&f, 0, sizeof f);
    f.px = P6(x);
    f.py = P6(y);
    f.len = P6(38);
    f.rb = P6(4.5);
    f.rt = P6(2.5);
    f.rest = left ? DEG(30) : DEG(150);
    f.up = left ? DEG(-28) : DEG(208);
    f.acc = 27584;           /* the table's values (tools/gen.py) */
    f.dec = 6000;
    f.brake = 64000;
    f.rise = 3;
    f.wmax = 96000;
    f.mat = M_FLIP;
    f.layers = 1;
    return f;
}

/* upper tangent point from P to the circle (C, r): where an inlane guide meets a flipper base */
static void tangent(double px_, double py_, double cx, double cy, double r, double *tx, double *ty)
{
    double d = hypot(px_ - cx, py_ - cy), th = atan2(py_ - cy, px_ - cx), al = acos(r / d);
    double x1 = cx + r * cos(th + al), y1 = cy + r * sin(th + al);
    double x2 = cx + r * cos(th - al), y2 = cy + r * sin(th - al);
    if (y1 < y2) { *tx = x1; *ty = y1; } else { *tx = x2; *ty = y2; }
}

static void lower_table(void)
{
    double tx, ty;
    FlipperDef f;
    ndefs = 0;
    seg(4, 0, 4, 322, M_WALL, 0, 0);                          /* outer walls */
    seg(238, 0, 238, 322, M_WALL, 0, 0);
    seg(4, 4, 238, 4, M_WALL, 0, 0);
    seg(18, 226, 18, 252, M_WALL, 0, 0);                      /* left inlane guide */
    tangent(18, 252, LPX, PY, 4.5, &tx, &ty);
    seg(18, 252, tx, ty, M_WALL, 0, 0);                     /* runs tangent into the flipper base */
    seg(224, 226, 224, 252, M_WALL, 0, 0);                    /* right inlane guide */
    tangent(224, 252, RPX, PY, 4.5, &tx, &ty);
    seg(224, 252, tx, ty, M_WALL, 0, 0);
    world(256, 322);
    f = flipdef(LPX, PY, 1);
    lflip = phys_add_flipper(&W, &f);
    f = flipdef(RPX, PY, 0);
    rflip = phys_add_flipper(&W, &f);
}

/* point on the left flipper at distance t (px) from the pivot, at rest, on its top face */
static void on_left_flipper(double t, double *x, double *y)
{
    double a = 30 * 3.14159265 / 180;
    *x = LPX + t * cos(a) + sin(a) * (4.5 - 2.0 * t / 38 + BALL_R + 0.2);
    *y = PY + t * sin(a) - cos(a) * (4.5 - 2.0 * t / 38 + BALL_R + 0.2);
}

static void test_flipper_shots(void)
{
    /* The real use: a ball rolls down the left inlane onto the resting flipper; the player flips
       at different moments. Early flips (ball near the pivot) and late flips (near the tip) must
       aim at different places, and a well-timed flip must give a strong shot. */
    int k, steps_up = 0, best_k = 0;
    double sp[16], ang[16], amin = 999, amax = -999, best = 0;
    for (k = 0; k < 16; k++) {
        int i, flipped = 0;
        lower_table();
        phys_add_ball(&W, FXPX(22), FXPX(240), VPS(144), 0);
        if (k == 6) dump_begin("shot");
        for (i = 0; i < SEC(3); i++) {
            /* flip when the ball has rolled to a given distance along the flipper */
            double dx = px(W.ball[0].x) - LPX, dy = px(W.ball[0].y) - PY;
            double along = dx * 0.866 + dy * 0.5;
            if (!flipped && along > 2 + k * 2.4 && dy > -12) {
                phys_flipper_button(&W, lflip, 1);
                flipped = i;
            }
            step(1);
            if (k == 0 && flipped && !steps_up && W.flip[lflip].ang == W.flip[lflip].a_up)
                steps_up = i + 1 - flipped;
            if (flipped && i > flipped + SEC(0.125))
                break;
        }
        dump_end();
        sp[k] = spd(&W.ball[0]) * RATE;
        ang[k] = atan2(-px(W.ball[0].vy), px(W.ball[0].vx)) * 180 / 3.14159265;
        if (sp[k] > 600 && px(W.ball[0].vy) < 0) {
            if (ang[k] < amin) amin = ang[k];
            if (ang[k] > amax) amax = ang[k];
        }
        if (sp[k] > best) { best = sp[k]; best_k = k; }
        if (verbose)
            printf("   flip at %4.1f px along: speed %4.0f px/s, angle %5.1f deg\n", 2 + k * 2.4, sp[k], ang[k]);
    }
    check(best > 1200 && amax - amin > 25, "flipper shots (timing)",
          "flip %d steps (%.0f ms); best %.0f px/s at %.0f px along; aim spans %.0f..%.0f deg",
          steps_up, steps_up * 1000 / RATE, best, 2 + best_k * 2.4, amin, amax);
}

static void test_cradle(void)
{
    /* Hold the left flipper up and drop a ball on it: it must roll down to the inlane corner
       and come to rest there (a cradle), not fall through. */
    int i;
    double x, y;
    lower_table();
    phys_flipper_button(&W, lflip, 1);
    step(SEC(0.083));
    phys_add_ball(&W, FXPX(LPX + 30), FXPX(220), 0, 0);
    dump_begin("cradle");
    step(SEC(4));
    dump_end();
    x = px(W.ball[0].x);
    y = px(W.ball[0].y);
    check(W.ball[0].state == BALL_FREE && spd(&W.ball[0]) * RATE < 10 && x < LPX + 12 && y < PY,
          "cradle (trap)", "ball at rest at (%.1f, %.1f), speed %.1f px/s, %d still steps",
          x, y, spd(&W.ball[0]) * RATE, (int)W.ball[0].still);
    (void)i;
}

static double catch_rebound(double release_y)
{
    /* rebound speed in the first 60 ms after the ball first touches the flipper; release_y < 0
       means the flipper stays up */
    int i, released = 0, hit = -1;
    double best = 0;
    lower_table();
    phys_flipper_button(&W, lflip, 1);
    step(SEC(0.083));
    phys_add_ball(&W, FXPX(LPX + 28), FXPX(150), 0, VPS(1060));
    for (i = 0; i < SEC(2); i++) {
        int e;
        if (release_y > 0 && !released && px(W.ball[0].y) > release_y) {
            phys_flipper_button(&W, lflip, 0);
            released = 1;
        }
        step(1);
        for (e = 0; e < W.nev; e++)
            if (W.ev[e].type == EV_FLIPHIT && hit < 0)
                hit = i;
        if (hit < 0 && px(W.ball[0].vy) < 0)
            hit = i;
        if (hit >= 0 && i <= hit + SEC(0.06) && -px(W.ball[0].vy) > best)
            best = -px(W.ball[0].vy);
    }
    return best * RATE;
}

static void test_live_catch(void)
{
    /* A ball falls fast onto the raised left flipper. Held: it bounces back. Dropping the
       flipper just before the impact (a live catch) must kill most of the bounce: tried at
       several release moments, the best one must keep the rebound under 60%. */
    double held = catch_rebound(-1), best = 1e9, best_y = 0, y;
    for (y = 200; y <= 262; y += 2) {
        double r = catch_rebound(y);
        if (r < best) {
            best = r;
            best_y = y;
        }
    }
    check(best < held * 0.6, "live catch", "rebound %.0f px/s held, %.0f px/s when dropped as the ball is %.0f px above it",
          held, best, 266 - best_y);
}

static void test_tap_pass(void)
{
    /* Ball cradled on the right flipper. Release it; as it starts to roll down the flipper, a
       one-tick tap (8 steps, the shortest press the game can read) near the base, where the
       flipper surface moves slowly, pops it gently over to the left flipper, held up to catch
       it. Tried at several distances along the flipper; it must work at some of them. */
    int k, works = 0;
    char ok_at[128] = "";
    for (k = 0; k < 10; k++) {
        int i, tapped = 0;
        double at = 3 + k * 1.5, x, y;
        lower_table();
        phys_flipper_button(&W, rflip, 1);
        step(SEC(0.083));
        phys_add_ball(&W, FXPX(RPX - 26), FXPX(230), 0, 0);
        step(SEC(3));                        /* cradled */
        phys_flipper_button(&W, lflip, 1);    /* ready to catch on the left */
        phys_flipper_button(&W, rflip, 0);
        if (k == 5) dump_begin("tappass");
        for (i = 0; i < SEC(5); i++) {
            double along = -(px(W.ball[0].x) - RPX) * 0.866 + (px(W.ball[0].y) - PY) * 0.5;
            if (!tapped && along > at) {
                phys_flipper_button(&W, rflip, 1);
                tapped = i;
            }
            if (tapped && i == tapped + STEPS_PER_TICK)   /* one tick: the shortest press */
                phys_flipper_button(&W, rflip, 0);
            step(1);
        }
        dump_end();
        x = px(W.ball[0].x);
        y = px(W.ball[0].y);
        if (W.ball[0].state == BALL_FREE && x < 121 && y > 250 && spd(&W.ball[0]) * RATE < 24) {
            works++;
            sprintf(ok_at + strlen(ok_at), " %.1f", at);
        }
        if (verbose)
            printf("   tap at %.1f px along: ball ends (%.1f, %.1f)\n", at, x, y);
    }
    check(works >= 2, "tap pass", "the pass reaches the left cradle when tapped at%s px along the flipper", ok_at);
}

static void test_flipper_tunnel(void)
{
    /* Top-speed balls at a flipper while it swings up, aimed at points along it from all
       directions. After every step the ball may overlap the flipper body by at most 1 px (the
       contact pushes it out); a ball that went through would show up as a deep overlap. */
    int k, a, delay, bad = 0, shots = 0;
    double worst = 0;
    for (k = 0; k < 9; k++)
        for (a = 0; a < 360; a += 15)
            for (delay = 0; delay < SEC(0.025); delay += SEC(0.006) > 0 ? SEC(0.006) : 1) {
                double t = 4 + k * 4.2, x, y, ang = a * 3.14159265 / 180;
                int i;
                lower_table();
                x = LPX + t * cos(30 * 3.14159265 / 180) - cos(ang) * 16;
                y = PY + t * sin(30 * 3.14159265 / 180) - sin(ang) * 16;
                W.gravity = 0;
                phys_add_ball(&W, (fx)(x * 65536), (fx)(y * 65536), (fx)(cos(ang) * VPS(TOPV)), (fx)(sin(ang) * VPS(TOPV)));
                shots++;
                for (i = 0; i < SEC(0.083); i++) {
                    double dx, dy, ux, uy, tt, r, d, pen;
                    if (i == delay)
                        phys_flipper_button(&W, lflip, 1);
                    phys_step(&W);
                    W.nev = 0;
                    if (W.ball[0].state != BALL_FREE)
                        break;
                    ux = W.flip[lflip].ux / 16384.0;
                    uy = W.flip[lflip].uy / 16384.0;
                    dx = px(W.ball[0].x) - LPX;
                    dy = px(W.ball[0].y) - PY;
                    tt = dx * ux + dy * uy;
                    if (tt < 0) tt = 0;
                    if (tt > 38) tt = 38;
                    r = 4.5 - 2.0 * tt / 38;
                    d = sqrt((dx - tt * ux) * (dx - tt * ux) + (dy - tt * uy) * (dy - tt * uy));
                    pen = r + BALL_R - d;
                    if (pen > worst) worst = pen;
                    if (pen > 1.0) {
                        bad++;
                        break;
                    }
                }
            }
    check(bad == 0, "no tunnelling: moving flipper",
          "%d shots at a swinging flipper, %d overlapped > 1 px (worst overlap after a step %.2f px)",
          shots, bad, worst);
}

static void test_bumper_sling(void)
{
    int i, kicks = 0, slings = 0;
    double best = 0;
    ndefs = 0;
    seg(4, 4, 4, 318, M_WALL, 0, 0);
    seg(236, 4, 236, 318, M_WALL, 0, 0);
    seg(4, 4, 236, 4, M_WALL, 0, 0);
    circle(120, 120, 11, M_BUMPER, SF_BUMPER, 7);
    seg(60, 220, 100, 280, M_SLING, SF_KICK, 9);
    world(256, 322);
    phys_add_ball(&W, FXPX(121), FXPX(60), 0, 0);
    for (i = 0; i < SEC(3); i++) {
        int e;
        phys_step(&W);
        for (e = 0; e < W.nev; e++) {
            if (W.ev[e].type == EV_BUMPER) kicks++;
            if (W.ev[e].type == EV_SLING) slings++;
        }
        W.nev = 0;
        if (spd(&W.ball[0]) > best) best = spd(&W.ball[0]);
    }
    check(kicks >= 1 && best * RATE > 960, "pop bumper kicks", "%d bumper kicks, top speed %.0f px/s", kicks, best * RATE);
    (void)slings;
}

static void test_plunger(void)
{
    /* variable strength: the launch speed must grow with the pull */
    int pulls[3] = {4, 10, 16}, k;
    double v[3];
    for (k = 0; k < 3; k++) {
        int i;
        ndefs = 0;
        seg(238, 20, 238, 322, M_WALL, 0, 0);
        seg(252, 20, 252, 322, M_WALL, 0, 0);
        world(256, 322);
        phys_set_plunger(&W, P6(238), P6(252), P6(300), P6(16), 1 << 15, 57600);
        phys_add_ball(&W, FXPX(245), FXPX(295), 0, 0);
        step(SEC(0.2));
        phys_plunger_button(&W, 1);
        step(pulls[k] * 2);          /* 1/2 px per step */
        phys_plunger_button(&W, 0);
        v[k] = 0;
        for (i = 0; i < SEC(0.4); i++) {
            step(1);
            if (-px(W.ball[0].vy) > v[k]) v[k] = -px(W.ball[0].vy);
        }
    }
    check(v[0] < v[1] && v[1] < v[2] && v[2] * RATE > 1440, "plunger strength",
          "pull 4/10/16 px: launch %.0f / %.0f / %.0f px/s", v[0] * RATE, v[1] * RATE, v[2] * RATE);
}

static void test_bowl(void)
{
    /* The gradient-descent bowl: a parabolic dish (pull towards the centre) with a sink hole in
       the middle. Balls rolling in slowly spiral down into the hole; fast balls are bent but
       pass over it. */
    int k, slow_in = 0, fast_in = 0;
    double t_slow = 0;
    for (k = 0; k < 16; k++) {
        int i, sunk = -1, fast = k >= 8;
        double x0 = 100 + (k % 8) * 5, v = (fast ? 960.0 : 72.0) / RATE;
        ShapeDef *d;
        ndefs = 0;
        d = &defs[ndefs++];
        memset(d, 0, sizeof *d);
        d->kind = S_DISC; d->flags = SF_BOWL; d->layers = 1; d->x0 = P6(120); d->y0 = P6(150); d->x1 = P6(22);
        d = &defs[ndefs++];
        memset(d, 0, sizeof *d);
        d->kind = S_DISC; d->flags = SF_SINK; d->layers = 1; d->id = 5; d->x0 = P6(120); d->y0 = P6(150); d->x1 = P6(5);
        world(256, 322);
        phys_add_ball(&W, FXPX(x0), FXPX(118), (fx)(v * 0.3 * 65536), (fx)(v * 65536));
        if (k == 2) dump_begin("bowl");
        for (i = 0; i < SEC(6); i++) {
            int e;
            step(1);
            for (e = 0; e < W.nev; e++)
                if (W.ev[e].type == EV_SINK && sunk < 0) sunk = i;
            W.nev = 0;
            if (W.ball[0].state != BALL_FREE) break;
        }
        dump_end();
        if (sunk >= 0) {
            if (fast) fast_in++;
            else { slow_in++; t_slow += sunk / RATE; }
        }
    }
    check(slow_in >= 6 && fast_in <= 2, "gradient-descent bowl",
          "slow balls sunk: %d of 8 (mean %.2f s), fast balls sunk: %d of 8", slow_in,
          slow_in ? t_slow / slow_in : 0.0, fast_in);
}

static void test_determinism(void)
{
    uint32_t h[2];
    int run;
    for (run = 0; run < 2; run++) {
        int i;
        rng = 777;
        lower_table();
        phys_add_ball(&W, FXPX(60), FXPX(30), VPS(625), 0);
        phys_add_ball(&W, FXPX(180), FXPX(40), VPS(-337), 0);
        for (i = 0; i < SEC(30); i++) {
            if (i % 19 == 0) phys_flipper_button(&W, lflip, frand() < 0.5);
            if (i % 21 == 0) phys_flipper_button(&W, rflip, frand() < 0.5);
            phys_step(&W);
            W.nev = 0;
        }
        h[run] = phys_hash(&W);
    }
    check(h[0] == h[1], "determinism", "state hash after 30 s: %08x / %08x", h[0], h[1]);
}

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump_name = argv[++i];
    }
    if (dump_name) {
        /* only run the scenario that produces the dump */
        if (!strcmp(dump_name, "shot")) test_flipper_shots();
        else if (!strcmp(dump_name, "cradle")) test_cradle();
        else if (!strcmp(dump_name, "tappass")) test_tap_pass();
        else if (!strcmp(dump_name, "bowl")) test_bowl();
        return 0;
    }
    test_energy_box();
    test_energy_gravity();
    test_energy_damped();
    test_tunnel_walls();
    test_tunnel_box();
    test_flipper_tunnel();
    test_flipper_shots();
    test_cradle();
    test_live_catch();
    test_tap_pass();
    test_bumper_sling();
    test_plunger();
    test_bowl();
    test_determinism();
    printf("%s: %d failed\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails != 0;
}

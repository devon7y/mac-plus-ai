/* Orbit Duel: the deterministic simulation and the computer pilot. See sim.h. */
#include "sim.h"

const int16_t sin_q14[256] = {
    0, 402, 804, 1205, 1606, 2006, 2404, 2801, 3196, 3590, 3981, 4370, 4756, 5139, 5520, 5897,
    6270, 6639, 7005, 7366, 7723, 8076, 8423, 8765, 9102, 9434, 9760, 10080, 10394, 10702, 11003, 11297,
    11585, 11866, 12140, 12406, 12665, 12916, 13160, 13395, 13623, 13842, 14053, 14256, 14449, 14635, 14811, 14978,
    15137, 15286, 15426, 15557, 15679, 15791, 15893, 15986, 16069, 16143, 16207, 16261, 16305, 16340, 16364, 16379,
    16384, 16379, 16364, 16340, 16305, 16261, 16207, 16143, 16069, 15986, 15893, 15791, 15679, 15557, 15426, 15286,
    15137, 14978, 14811, 14635, 14449, 14256, 14053, 13842, 13623, 13395, 13160, 12916, 12665, 12406, 12140, 11866,
    11585, 11297, 11003, 10702, 10394, 10080, 9760, 9434, 9102, 8765, 8423, 8076, 7723, 7366, 7005, 6639,
    6270, 5897, 5520, 5139, 4756, 4370, 3981, 3590, 3196, 2801, 2404, 2006, 1606, 1205, 804, 402,
    0, -402, -804, -1205, -1606, -2006, -2404, -2801, -3196, -3590, -3981, -4370, -4756, -5139, -5520, -5897,
    -6270, -6639, -7005, -7366, -7723, -8076, -8423, -8765, -9102, -9434, -9760, -10080, -10394, -10702, -11003, -11297,
    -11585, -11866, -12140, -12406, -12665, -12916, -13160, -13395, -13623, -13842, -14053, -14256, -14449, -14635, -14811, -14978,
    -15137, -15286, -15426, -15557, -15679, -15791, -15893, -15986, -16069, -16143, -16207, -16261, -16305, -16340, -16364, -16379,
    -16384, -16379, -16364, -16340, -16305, -16261, -16207, -16143, -16069, -15986, -15893, -15791, -15679, -15557, -15426, -15286,
    -15137, -14978, -14811, -14635, -14449, -14256, -14053, -13842, -13623, -13395, -13160, -12916, -12665, -12406, -12140, -11866,
    -11585, -11297, -11003, -10702, -10394, -10080, -9760, -9434, -9102, -8765, -8423, -8076, -7723, -7366, -7005, -6639,
    -6270, -5897, -5520, -5139, -4756, -4370, -3981, -3590, -3196, -2801, -2404, -2006, -1606, -1205, -804, -402,
};
static const uint8_t atan_tab[65] = {
    0, 1, 1, 2, 3, 3, 4, 4, 5, 6, 6, 7, 8, 8, 9, 9,
    10, 11, 11, 12, 12, 13, 13, 14, 15, 15, 16, 16, 17, 17, 18, 18,
    19, 19, 20, 20, 21, 21, 22, 22, 23, 23, 24, 24, 25, 25, 25, 26,
    26, 27, 27, 27, 28, 28, 29, 29, 29, 30, 30, 30, 31, 31, 31, 32,
    32,
};

/* ---- tuning (per step; 30 steps per second) --------------------------------------------- */
#define WFP ((int32_t)ARENA_W << FP)
#define HFP ((int32_t)ARENA_H << FP)
#define PX(v) ((int16_t)((v) >> FP))
#define ROT 6                       /* rotation per step (256 = full turn): 1.4 s per turn */
#define ACC 205                     /* thrust: 0.05 px/step^2 */
#define VMAX16 96                   /* speed limit in 1/16 px per step (6 px/step) */
#define G_UNITS 1024000L            /* gravity a = G / r^2 with G = 250 px^3/step^2, in FP units */
#define STAR_KILL 12                /* px: a ship this close to the star burns up */
#define STAR_EAT 8                  /* px: torpedoes vanish this close to the star */
#define RELOAD 8
#define HIT_R 7                     /* px: torpedo hits ship */
#define SHIP_R 10                   /* px: ships collide */
#define READY_T 24                  /* steps the ships wait before a round starts */
#define ROUND_END_T 75              /* steps from the first explosion to the end of the round */
#define SPAWN_R 140                 /* px from the star */
#define ORBIT_V 5473                /* circular orbit speed at SPAWN_R, FP units per step */

static int32_t grav_k[300];         /* G * 4096 / r^3 for r in px (r clamped >= 12) */

static void tables(void)
{
    int16_t r;
    if (grav_k[1])
        return;
    for (r = 0; r < 300; r++) {
        int32_t rr = r < 12 ? 12 : r;
        grav_k[r] = (int32_t)((uint32_t)G_UNITS * 4096UL / (uint32_t)(rr * rr * rr));
    }
}

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

int16_t angle_of(int32_t dx, int32_t dy)
{
    int32_t x = dx, y = -dy, ax, ay;
    int16_t a;
    if (x == 0 && y == 0)
        return 0;
    ax = x < 0 ? -x : x;
    ay = y < 0 ? -y : y;
    if (ax >= ay)
        a = atan_tab[(ay * 64) / ax];
    else
        a = 64 - atan_tab[(ax * 64) / ay];
    if (x < 0)
        a = 128 - a;
    if (y < 0)
        a = 256 - a;
    return a & 255;
}

static uint32_t rnd(Sim *s)
{
    s->rng = s->rng * 1103515245UL + 12345UL;
    return s->rng >> 16;                    /* the high bits (an LCG's low bits repeat quickly) */
}

static int32_t wrapx(int32_t x) { return x < 0 ? x + WFP : x >= WFP ? x - WFP : x; }
static int32_t wrapy(int32_t y) { return y < 0 ? y + HFP : y >= HFP ? y - HFP : y; }

/* shortest wrapped distance a - b in px */
static int16_t wdx(int32_t a, int32_t b)
{
    int16_t d = PX(a) - PX(b);
    if (d < -ARENA_W / 2) d += ARENA_W;
    else if (d >= ARENA_W / 2) d -= ARENA_W;
    return d;
}
static int16_t wdy(int32_t a, int32_t b)
{
    int16_t d = PX(a) - PX(b);
    if (d < -ARENA_H / 2) d += ARENA_H;
    else if (d >= ARENA_H / 2) d -= ARENA_H;
    return d;
}

/* pull towards the star (in 1/16 px for a smooth direction) */
static void gravity(int32_t x, int32_t y, int32_t *vx, int32_t *vy)
{
    int32_t dx = (x >> (FP - 4)) - STAR_X * 16, dy = (y >> (FP - 4)) - STAR_Y * 16;
    int32_t r = isqrt32((uint32_t)(dx * dx + dy * dy)) >> 4;
    int32_t k = grav_k[r < 300 ? r : 299];
    *vx -= (dx * k) >> 16;
    *vy -= (dy * k) >> 16;
}

static void new_round(Sim *s)
{
    int16_t i, k;
    for (i = 0; i < 2; i++) {
        Ship *p = &s->ship[i];
        int16_t keep = p->prev_in;
        for (k = 0; k < MAX_TORPS; k++)
            s->torp[i][k].life = 0;
        p->x = (int32_t)(i ? STAR_X + SPAWN_R : STAR_X - SPAWN_R) << FP;
        p->y = (int32_t)STAR_Y << FP;
        p->vx = 0;
        p->vy = i ? -ORBIT_V : ORBIT_V;     /* both orbit counter-clockwise on the screen */
        p->angle = i ? 128 : 0;             /* facing each other across the star */
        p->alive = 1;
        p->hyper = p->hyper_cool = p->jumps = 0;
        p->reload = 0;
        p->dead_t = p->thrust = p->cause = 0;
        p->prev_in = keep;
    }
    s->phase = PH_READY;
    s->phase_t = 0;
    s->round++;
    s->events |= EV_NEWROUND;
}

void sim_init(Sim *s, uint32_t seed, int16_t first_to)
{
    uint8_t *b = (uint8_t *)s;
    uint16_t i;
    tables();
    for (i = 0; i < sizeof *s; i++)
        b[i] = 0;
    s->rng = seed ? seed : 1;
    s->first_to = first_to;
    s->winner = 2;
    new_round(s);
}

static void kill(Sim *s, int16_t i, int16_t cause)
{
    Ship *p = &s->ship[i];
    if (!p->alive)
        return;
    p->alive = 0;
    p->dead_t = 0;
    p->cause = cause;
    p->thrust = 0;
    s->events |= EV_BOOM;
    if (s->phase == PH_PLAY) {
        s->phase = PH_ROUND_END;
        s->phase_t = 0;
    }
}

static void ship_update(Sim *s, int16_t i, uint8_t in)
{
    Ship *p = &s->ship[i];
    uint8_t edge = in & ~p->prev_in;
    int16_t a = p->angle, k;
    int32_t sx, sy;

    if (!p->alive) {
        if (p->dead_t < 1000)
            p->dead_t++;
        return;
    }
    if (p->reload)
        p->reload--;
    if (p->hyper_cool)
        p->hyper_cool--;
    p->thrust = 0;
    if (p->hyper) {
        if (--p->hyper == 0) {             /* back from hyperspace somewhere random */
            int16_t x = 0, y = 0, tries;
            for (tries = 0; tries < 8; tries++) {
                int16_t dx, dy;
                x = 20 + (int16_t)(rnd(s) % (ARENA_W - 40));
                y = 20 + (int16_t)(rnd(s) % (ARENA_H - 40));
                dx = x - STAR_X;
                dy = y - STAR_Y;
                if ((int32_t)dx * dx + (int32_t)dy * dy > 80L * 80)
                    break;
            }
            p->x = (int32_t)x << FP;
            p->y = (int32_t)y << FP;
            s->events |= EV_HYPEROUT;
            if ((int16_t)(rnd(s) & 7) < (p->jumps < 6 ? p->jumps : 6))
                kill(s, i, 4);              /* the jump went wrong: 1/8, 2/8, 3/8 ... */
        }
        return;
    }
    if (in & IN_LEFT)
        p->angle = (p->angle + ROT) & 255;
    if (in & IN_RIGHT)
        p->angle = (p->angle - ROT) & 255;
    a = p->angle;
    if (in & IN_THRUST) {
        p->vx += ((int32_t)COS_Q14(a) * ACC) >> 14;
        p->vy -= ((int32_t)SIN_Q14(a) * ACC) >> 14;
        p->thrust = 1;
    }
    if ((in & IN_FIRE) && p->reload == 0) {
        for (k = 0; k < MAX_TORPS; k++) {
            Torp *t = &s->torp[i][k];
            if (t->life)
                continue;
            t->x = wrapx(p->x + 2 * (int32_t)COS_Q14(a));       /* 8 px ahead of the centre */
            t->y = wrapy(p->y - 2 * (int32_t)SIN_Q14(a));
            t->vx = p->vx + COS_Q14(a);                          /* 4 px/step = 16384 units */
            t->vy = p->vy - SIN_Q14(a);
            t->life = TORP_LIFE;
            t->age = 0;
            p->reload = RELOAD;
            s->events |= EV_FIRE0 << i;
            break;
        }
    }
    if ((edge & IN_HYPER) && p->hyper_cool == 0) {
        p->hyper = HYPER_T;
        p->jumps++;
        p->hyper_cool = HYPER_COOL + HYPER_T;
        s->events |= EV_HYPER;
        return;
    }
    gravity(p->x, p->y, &p->vx, &p->vy);
    sx = p->vx >> 8;
    sy = p->vy >> 8;
    if (sx * sx + sy * sy > (int32_t)VMAX16 * VMAX16) {
        p->vx -= p->vx >> 4;
        p->vy -= p->vy >> 4;
    }
    p->x = wrapx(p->x + p->vx);
    p->y = wrapy(p->y + p->vy);
}

static void torps_update(Sim *s)
{
    int16_t i, k;
    for (i = 0; i < 2; i++)
        for (k = 0; k < MAX_TORPS; k++) {
            Torp *t = &s->torp[i][k];
            int16_t dx, dy;
            if (!t->life)
                continue;
            t->x = wrapx(t->x + t->vx);
            t->y = wrapy(t->y + t->vy);
            t->age++;
            t->life--;
            dx = PX(t->x) - STAR_X;
            dy = PX(t->y) - STAR_Y;
            if ((int32_t)dx * dx + (int32_t)dy * dy < STAR_EAT * STAR_EAT)
                t->life = 0;
        }
}

static void collisions(Sim *s)
{
    int16_t i, j, k, hit[2] = {0, 0};
    for (i = 0; i < 2; i++) {
        Ship *p = &s->ship[i];
        int16_t dx, dy;
        if (!p->alive || p->hyper)
            continue;
        dx = PX(p->x) - STAR_X;
        dy = PX(p->y) - STAR_Y;
        if ((int32_t)dx * dx + (int32_t)dy * dy < STAR_KILL * STAR_KILL) {
            hit[i] = 2;
            continue;
        }
        for (j = 0; j < 2 && !hit[i]; j++)
            for (k = 0; k < MAX_TORPS; k++) {
                Torp *t = &s->torp[j][k];
                if (!t->life || (j == i && t->age < 6))
                    continue;
                dx = wdx(t->x, p->x);
                dy = wdy(t->y, p->y);
                if ((int32_t)dx * dx + (int32_t)dy * dy < HIT_R * HIT_R) {
                    t->life = 0;
                    hit[i] = 1;
                    break;
                }
            }
    }
    if (!hit[0] && !hit[1] && s->ship[0].alive && s->ship[1].alive && !s->ship[0].hyper && !s->ship[1].hyper) {
        int16_t dx = wdx(s->ship[0].x, s->ship[1].x), dy = wdy(s->ship[0].y, s->ship[1].y);
        if ((int32_t)dx * dx + (int32_t)dy * dy < SHIP_R * SHIP_R)
            hit[0] = hit[1] = 3;
    }
    for (i = 0; i < 2; i++)
        if (hit[i])
            kill(s, i, hit[i]);
}

void sim_step(Sim *s, uint8_t in0, uint8_t in1)
{
    uint8_t in[2];
    int16_t i, start = 0, fire = 0;
    in[0] = in0;
    in[1] = in1;
    s->events = 0;
    for (i = 0; i < 2; i++) {
        uint8_t edge = in[i] & ~s->ship[i].prev_in;
        if (edge & IN_START) start = 1;
        if (edge & IN_FIRE) fire = 1;
    }
    if (s->paused) {
        if (start) {
            s->paused = 0;
            s->events |= EV_PAUSE;
        }
        goto done;
    }
    if (s->phase == PH_MATCH_END) {
        s->phase_t++;
        for (i = 0; i < 2; i++)
            if (!s->ship[i].alive && s->ship[i].dead_t < 1000)
                s->ship[i].dead_t++;
        if (s->phase_t > 45 && (start || fire)) {
            s->score[0] = s->score[1] = 0;
            s->matches++;
            new_round(s);
        }
        goto done;
    }
    if (start) {
        s->paused = 1;
        s->events |= EV_PAUSE;
        goto done;
    }
    if (s->phase == PH_READY) {
        if (++s->phase_t >= READY_T) {
            s->phase = PH_PLAY;
            s->phase_t = 0;
        }
        goto done;
    }
    for (i = 0; i < 2; i++)
        ship_update(s, i, in[i]);
    torps_update(s);
    collisions(s);
    if (s->phase == PH_ROUND_END && ++s->phase_t >= ROUND_END_T) {
        int16_t a0 = s->ship[0].alive, a1 = s->ship[1].alive;
        s->winner = a0 && !a1 ? 0 : a1 && !a0 ? 1 : 2;
        if (s->winner < 2)
            s->score[s->winner]++;
        s->events |= EV_ROUND;
        if (s->winner < 2 && s->score[s->winner] >= s->first_to) {
            s->phase = PH_MATCH_END;
            s->phase_t = 0;
            s->events |= EV_MATCH;
        } else
            new_round(s);
    }
done:
    for (i = 0; i < 2; i++)
        s->ship[i].prev_in = in[i];
    s->tick++;
}

/* A hash of every field, in a fixed order (so the host build gives the same value). */
#define H(v) (h = (h ^ (uint32_t)(v)) * 16777619UL)
uint32_t sim_checksum(const Sim *s)
{
    uint32_t h = 2166136261UL;
    int16_t i, k;
    H(s->rng); H(s->tick); H(s->phase); H(s->phase_t); H(s->paused); H(s->first_to);
    H(s->score[0]); H(s->score[1]); H(s->winner); H(s->round); H(s->matches); H(s->events);
    for (i = 0; i < 2; i++) {
        const Ship *p = &s->ship[i];
        H(p->x); H(p->y); H(p->vx); H(p->vy); H(p->angle); H(p->alive); H(p->hyper);
        H(p->hyper_cool); H(p->jumps); H(p->reload); H(p->dead_t); H(p->thrust); H(p->prev_in);
        H(p->cause);
        for (k = 0; k < MAX_TORPS; k++) {
            const Torp *t = &s->torp[i][k];
            H(t->x); H(t->y); H(t->vx); H(t->vy); H(t->life); H(t->age);
        }
    }
    return h;
}

/* ---- computer pilot ------------------------------------------------------------------- */

static uint16_t airnd(Ai *ai)
{
    ai->rng = ai->rng * 1103515245UL + 12345UL;
    return (uint16_t)(ai->rng >> 16);
}

void ai_init(Ai *ai, uint32_t seed, int16_t level, int16_t auto_restart)
{
    ai->rng = seed ? seed : 7;
    ai->level = level;
    ai->auto_restart = auto_restart;
    ai->hold = ai->last = ai->wobble = 0;
}

static int16_t adiff(int16_t to, int16_t from)
{
    return (int16_t)(((to - from + 128) & 255) - 128);
}

uint8_t ai_input(Ai *ai, const Sim *s, int16_t me)
{
    const Ship *m = &s->ship[me], *o = &s->ship[1 - me];
    uint8_t out = 0;
    int16_t desired = -1, thrust = 0, fire = 0, i, k, threat = 0, tc_best = 99;
    int32_t rx, ry, r2, vx16, vy16, radial, best_cx = 0, best_cy = 0;
    static const int16_t fire_pct[3] = {35, 70, 95}, hyper_pct[3] = {10, 35, 60};

    if (s->phase == PH_MATCH_END)
        return (ai->auto_restart && s->phase_t > 90 && (s->phase_t & 8)) ? IN_FIRE : 0;
    if (s->paused || !m->alive || m->hyper || s->phase == PH_READY)
        return 0;
    if (ai->hold > 0) {
        ai->hold--;
        return ai->last;
    }
    rx = PX(m->x) - STAR_X;
    ry = PX(m->y) - STAR_Y;
    r2 = rx * rx + ry * ry;
    vx16 = m->vx >> 8;
    vy16 = m->vy >> 8;
    radial = rx * vx16 + ry * vy16;

    /* torpedoes about to hit us (closest approach within 15 steps, closer than 10 px) */
    for (i = 0; i < 2; i++)
        for (k = 0; k < MAX_TORPS; k++) {
            const Torp *t = &s->torp[i][k];
            int32_t dx, dy, wx, wy, dot, ww, tc, cx, cy;
            if (!t->life || (i == me && t->age < 6))
                continue;
            dx = (int32_t)wdx(t->x, m->x) * 16;
            dy = (int32_t)wdy(t->y, m->y) * 16;
            wx = (t->vx - m->vx) >> 8;
            wy = (t->vy - m->vy) >> 8;
            dot = dx * wx + dy * wy;
            ww = wx * wx + wy * wy;
            if (dot >= 0 || ww == 0)
                continue;
            tc = -dot / ww;
            if (tc > 15 || tc > t->life)
                continue;
            cx = dx + wx * tc;
            cy = dy + wy * tc;
            if (cx * cx + cy * cy < 160L * 160 && tc < tc_best) {
                threat = 1;
                tc_best = (int16_t)tc;
                best_cx = cx;
                best_cy = cy;
            }
        }

    if (r2 < 60L * 60 || (r2 < 110L * 110 && radial < 0 && -radial > isqrt32((uint32_t)r2) * 12)) {
        /* falling into the star: thrust outwards and along the way we are going */
        int32_t r = isqrt32((uint32_t)r2) + 1, v = isqrt32((uint32_t)(vx16 * vx16 + vy16 * vy16)) + 1;
        desired = angle_of(rx * 64 / r + vx16 * 48 / v, ry * 64 / r + vy16 * 48 / v);
        thrust = 1;
    } else if (threat) {
        if (m->hyper_cool == 0 && m->jumps < (ai->level ? 2 : 1) && tc_best <= 4
            && (int16_t)(airnd(ai) % 100) < hyper_pct[ai->level]) {
            out = IN_HYPER;
            ai->last = out;
            ai->hold = 0;
            return out;
        }
        desired = angle_of(-best_cx, -best_cy);      /* get out of the torpedo's way */
        thrust = 1;
    } else if (o->alive && !o->hyper) {
        int32_t dx = wdx(o->x, m->x), dy = wdy(o->y, m->y);
        int32_t dist = isqrt32((uint32_t)(dx * dx + dy * dy));
        int32_t tf = ai->level ? dist / 4 : 0;
        int32_t ax = dx + (((o->vx - m->vx) >> 4) * tf >> (FP - 4));
        int32_t ay = dy + (((o->vy - m->vy) >> 4) * tf >> (FP - 4));
        int32_t speed = isqrt32((uint32_t)(vx16 * vx16 + vy16 * vy16));
        if ((s->tick & 15) == 0)
            ai->wobble = ai->level == 2 ? 0 : (int16_t)(airnd(ai) % (ai->level ? 7 : 17)) - (ai->level ? 3 : 8);
        desired = (angle_of(ax, ay) + ai->wobble) & 255;
        fire = dist < 170;
        thrust = (dist > 140 && speed < 40) || speed < 8;
    } else {
        /* the other ship is gone: line up on the centre and drift */
        desired = angle_of(-rx, -ry);
        thrust = 0;
    }
    if (desired >= 0) {
        int16_t d = adiff(desired, m->angle), ad = d < 0 ? -d : d;
        if (d > 3)
            out |= IN_LEFT;
        else if (d < -3)
            out |= IN_RIGHT;
        if (thrust && ad < 40)
            out |= IN_THRUST;
        if (fire && ad < (ai->level == 2 ? 5 : 8) && m->reload == 0
            && (int16_t)(airnd(ai) % 100) < fire_pct[ai->level])
            out |= IN_FIRE;
    }
    ai->last = out;
    ai->hold = ai->level == 0 ? 3 : ai->level == 1 ? 1 : 0;
    return out;
}

/* Rail Runner game logic. Mirrors runner/proto/game.py line by line (integer maths, same order),
 * which in turn uses the player physics of proto/sim.py that test_escape.py proves fair. */
#include "game.h"

#define COIN_Y 300          /* coins float this far above the route's feet height */
#define JET_FALL 24         /* max fall speed while invulnerable (gentle landing) */
#define SKY_LEAD (14 * U)   /* jetpack sky coins appear this far ahead */
#define SKY_STOP 100        /* ... and stop this many ticks before the jetpack ends */

static const short tier_speed[4] = TIER_SPEED_INIT;

static unsigned long rnd(Game *g)
{
    unsigned long x = g->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g->rng = x;
    return x;
}

static short below(Game *g, short n)
{
    return (short)((unsigned short)(rnd(g) >> 16) % (unsigned short)n);
}

static short speed_at(long z)
{
    long v;
#if SPEED_STEP_ZU < 32768 && VMAX - V0 < 32767
    /* same result as below with one divs.w instead of a 32-bit library division:
       the quotient fits 16 bits whenever it is below the cap */
    if (z >= (long)(VMAX - V0) * SPEED_STEP_ZU)
        return VMAX;
    if (z >= 0) {
        long q = z;
        __asm__("divs.w %1,%0" : "+d"(q) : "dmi"((short)SPEED_STEP_ZU) : "cc");
        return (short)(V0 + (short)q);
    }
#endif
    v = V0 + z / SPEED_STEP_ZU;
    return (short)(v > VMAX ? VMAX : v);
}

static short gap_rows(short v)
{
    return GAP_ROWS + (v - V0) / GAP_PER_V;
}

static short pick(Game *g, short v)
{
    short i, total = 0, r;
    for (i = 0; i < N_PATTERNS; i++)
        if (tier_speed[pat_defs[i].tier] <= v && i != g->last_pat)
            total += pat_defs[i].weight;
    r = below(g, total);
    for (i = 0; i < N_PATTERNS; i++) {
        if (tier_speed[pat_defs[i].tier] <= v && i != g->last_pat) {
            r -= pat_defs[i].weight;
            if (r < 0) {
                g->last_pat = i;
                return i;
            }
        }
    }
    return 0;
}

static void spawn_pattern(Game *g)
{
    short v = speed_at(g->z);
    short i = pick(g, v), k, first_coin = g->ncoin;
    const PatDef *d = &pat_defs[i];
    long zs = g->next_pat;

    for (k = 0; k < d->nobj; k++) {
        const PatObj *po = &pat_objs[d->obj + k];
        long a = zs + (long)po->r0 * U, b = zs + (long)po->r1 * U;
        Obj *o;
        switch (po->kind) {
        case K_LOW:  a += (U - LOW_D) / 2;  b = a + LOW_D;  break;
        case K_HIGH: a += (U - HIGH_D) / 2; b = a + HIGH_D; break;
        case K_WALL: a += (U - WALL_D) / 2; b = a + WALL_D; break;
        case K_ONCOMING: {
            long t_meet = (a - g->z) / v, len = b - a;
            a = a + ONCOMING_V * t_meet;
            b = a + len;
            break;
        }
        }
        if (g->nobj >= MAX_OBJ)
            continue;
        o = &g->obj[g->nobj++];
        o->kind = po->kind;
        o->lane = po->lane;
        o->z0 = a;
        o->z1 = b;
    }
    for (k = 0; k < d->ncoin; k++) {
        const PatCoin *pc = &pat_coins[d->coin + k];
        Coin *c;
        if (g->ncoin >= MAX_COIN)
            break;
        c = &g->coin[g->ncoin++];
        c->z = zs + (long)pc->row * U + U / 2;
        c->lane = pc->lane;
        c->y = pc->y + COIN_Y;
        c->type = 0;
    }
    if (below(g, POWERUP_EVERY) == 0 && g->ncoin > first_coin) {
        short n = g->ncoin - first_coin;
        short j = below(g, n);
        g->coin[first_coin + j].type = 1 + below(g, N_PU - 1);
    }
    g->next_pat += (long)(d->rows + gap_rows(v)) * U;
}

short game_collision_lane(const Player *p)
{
    return p->sw > SWITCH_T / 2 ? p->frm : p->lane;
}

/* ground height and high-bar presence per lane over [z - PZ, z + PZ] */
static void env_at(Game *g, short *gnd, char *bar)
{
    long lo = g->z - PZ, hi = g->z + PZ;
    short i;
    gnd[0] = gnd[1] = gnd[2] = 0;
    bar[0] = bar[1] = bar[2] = 0;
    for (i = 0; i < g->nobj; i++) {
        const Obj *o = &g->obj[i];
        short h;
        if (o->z1 <= lo || o->z0 >= hi)
            continue;
        if (o->kind == K_HIGH) {
            bar[o->lane] = 1;
            continue;
        }
        if (o->kind == K_RAMP) {
            long top = hi < o->z1 ? hi : o->z1;
            h = (short)((long)TRAIN_H * (top - o->z0) / (o->z1 - o->z0));
        } else if (o->kind == K_LOW) {
            h = LOW_H;
        } else {
            h = TRAIN_H;
        }
        if (h > gnd[o->lane])
            gnd[o->lane] = h;
    }
}

static short player_step(Game *g, short in, const short *gnd, const char *bar)
{
    Player *p = &g->p;
    short jump_v = g->sneak_t ? JUMP_V_HI : JUMP_V;
    short inv = g->invuln_t > 0 || g->jet_t > 0;
    short cl, yprev, gc, h;

    if ((in == IN_LEFT || in == IN_RIGHT) && p->sw == 0) {
        short t = in == IN_LEFT ? p->lane - 1 : p->lane + 1;
        if (t >= 0 && t < LANES) {
            if (!inv && (gnd[t] > p->y + STEP_TOL ||
                         (bar[t] && p->y < HIGH_T && p->y + STAND_H > HIGH_B)))
                return EV_STUMBLE;
            p->frm = p->lane;
            p->lane = t;
            p->sw = SWITCH_T;
            g->events |= 64;
        }
    } else if (in == IN_JUMP && p->gnd) {
        p->vy = jump_v;
        p->gnd = 0;
        p->roll = 0;
        g->events |= 2;
    } else if (in == IN_ROLL) {
        if (p->gnd) {
            p->roll = ROLL_T;
            g->events |= 4;
        } else if (p->vy > -DIVE_V) {
            p->vy = -DIVE_V;
            p->roll = ROLL_T;
            g->events |= 4;
        }
    }
    if (p->sw)
        p->sw--;
    cl = game_collision_lane(p);
    if (cl != p->cl && !inv && gnd[cl] > p->y + STEP_TOL)
        return EV_STUMBLE;
    p->cl = cl;
    if (g->jet_t > 0) {
        if (p->y < JET_Y) {
            p->y += JET_RISE;
            if (p->y > JET_Y)
                p->y = JET_Y;
        }
        p->vy = 0;
        p->gnd = 0;
        p->roll = 0;
        return EV_OK;
    }
    yprev = p->y;
    p->y += p->vy;
    p->vy -= G;
    if (inv && p->vy < -JET_FALL)
        p->vy = -JET_FALL;
    gc = gnd[cl];
    if (p->y <= gc) {
        if (yprev < gc - STEP_TOL && !inv)
            return EV_CRASH;
        p->y = gc;
        p->vy = 0;
        p->gnd = 1;
    } else {
        p->gnd = 0;
    }
    if (p->roll)
        p->roll--;
    h = p->roll ? ROLL_H : STAND_H;
    if (!inv && bar[cl] && p->y < HIGH_T && p->y + h > HIGH_B)
        return EV_CRASH;
    return EV_OK;
}

static void collect(Game *g, Coin *c)
{
    short mult = g->mult_t ? 2 : 1;
    g->events |= c->type ? 32 : 1;
    switch (c->type) {
    case 0:
        g->coins++;
        g->score += COIN_POINTS * mult;
        break;
    case PU_JETPACK:
        g->jet_t = JET_T;
        g->next_sky = g->z + 4 * U;
        break;
    case PU_SNEAKERS: g->sneak_t = SNEAKERS_T; break;
    case PU_MAGNET:   g->magnet_t = MAGNET_T; break;
    case PU_MULT:     g->mult_t = MULT_T; break;
    case PU_BOARD:    if (g->boards < 9) g->boards++; break;
    }
    if (c->type)
        g->pu_got = c->type;
}

void game_init(Game *g, unsigned long seed)
{
    char *b = (char *)g;
    unsigned short i;
    for (i = 0; i < sizeof(Game); i++)
        b[i] = 0;
    g->rng = seed ? seed : 1;
    g->v = V0;
    g->p.lane = g->p.frm = g->p.cl = 1;
    g->p.gnd = 1;
    g->next_pat = START_CLEAR;
    g->last_pat = -1;
    g->chase_t = CHASE_START_T;
    g->boards = 1;
}

void game_tick(Game *g, short in)
{
    Player *p = &g->p;
    short gnd[3], i, ev, cl;
    char bar[3];

    g->ticks++;
    g->events = 0;
    if (g->state != ST_RUN) {
        g->state_t++;
        return;
    }
    /* 1. advance */
    g->v = speed_at(g->z);
    g->z += g->v;
    /* 2. spawn patterns that came within draw distance */
    while (g->z + DRAW_DIST >= g->next_pat)
        spawn_pattern(g);
    if (g->jet_t > SKY_STOP) {
        while (g->z + SKY_LEAD >= g->next_sky) {
            if (g->ncoin < MAX_COIN) {
                Coin *c = &g->coin[g->ncoin++];
                c->z = g->next_sky;
                c->lane = (short)(((g->next_sky >> 10) / 10) % 3);
                c->y = JET_Y + COIN_Y;
                c->type = 0;
            }
            g->next_sky += U;
        }
    }
    /* 3. move the world, drop what is far behind */
    for (i = 0; i < g->nobj;) {
        Obj *o = &g->obj[i];
        if (o->kind == K_ONCOMING) {
            o->z0 -= ONCOMING_V;
            o->z1 -= ONCOMING_V;
        }
        if (o->z1 <= g->z - 4 * U) {
            *o = g->obj[--g->nobj];
            continue;
        }
        i++;
    }
    /* 4. hoverboard */
    if (in == IN_BOARD) {
        if (g->boards > 0 && g->hover_t == 0) {
            g->boards--;
            g->hover_t = HOVER_T;
            g->events |= 32;
        }
        in = IN_NONE;
    }
    /* 5. player */
    env_at(g, gnd, bar);
    ev = player_step(g, in, gnd, bar);
    if (ev == EV_STUMBLE) {
        if (p->sw) {
            p->lane = p->frm;
            p->sw = 0;
            p->cl = p->lane;
        }
        g->stumble_t = STUMBLE_T;
        g->events |= 16;
        if (g->chase_t > 0) {
            g->state = ST_CAUGHT;
            g->state_t = 0;
            return;
        }
        g->chase_t = CHASE_T;
    } else if (ev == EV_CRASH) {
        if (g->hover_t > 0) {
            g->hover_t = 0;
            g->invuln_t = INVULN_T;
            p->y = gnd[p->cl];
            p->vy = 0;
            p->gnd = 1;
            g->events |= 8;
        } else {
            g->state = ST_CRASHED;
            g->state_t = 0;
            g->events |= 8;
            return;
        }
    }
    /* 6. coins and power-ups */
    cl = p->cl;
    {
    /* Same order and effects as "for (i = 0; i < ncoin;)" over the array (Python mirror): coins
     * far ahead need nothing, so a tight inner loop skips them. collect() never moves the player. */
    Coin *c = g->coin;
    const long gz = g->z;
    for (;;) {
        Coin *e = g->coin + g->ncoin;
        long dz;
        while (c < e && c->z - gz > COIN_R_Z)
            c++;
        if (c >= e)
            break;
        dz = c->z - gz;
        if (dz < -COIN_R_Z) {
            *c = g->coin[--g->ncoin];
            continue;
        }
        if (dz <= COIN_R_Z) {
            short dy = c->y - (p->y + COIN_Y);
            short hit;
            if (dy < 0)
                dy = -dy;
            if (g->magnet_t && c->type == 0)
                hit = 1;
            else
                hit = c->lane == cl && dy <= COIN_R_Y;
            if (hit) {
                collect(g, c);
                *c = g->coin[--g->ncoin];
                continue;
            }
        }
        c++;
    }
    }
    /* 7. timers and score */
    if (g->stumble_t) g->stumble_t--;
    if (g->chase_t) g->chase_t--;
    if (g->invuln_t) g->invuln_t--;
    if (g->jet_t) {
        g->jet_t--;
        if (g->jet_t == 0) {
            g->invuln_t = INVULN_T;
            p->vy = 0;
        }
    }
    if (g->sneak_t) g->sneak_t--;
    if (g->magnet_t) g->magnet_t--;
    if (g->mult_t) g->mult_t--;
    if (g->hover_t) g->hover_t--;
    {
        long u = g->z >> 10;
        short mult = g->mult_t ? 2 : 1;
        while (g->last_u < u) {
            g->last_u++;
            g->score += mult;
        }
    }
}

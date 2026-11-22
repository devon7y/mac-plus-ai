/* Tank Duel: the deterministic fixed-point game simulation. See sim.h; twin.py mirrors every line. */
#include "sim.h"

static const short TUR_OFF[7] = {-24, -8, -2, 0, 2, 8, 24};

static short iabs(short v) { return v < 0 ? -v : v; }
static short clamp127(long v) { return v > 127 ? 127 : v < -127 ? -127 : (short)v; }

/* rounded Q14 product: (v * q + 8192) >> 14 */
static short mulq(short v, short q) { return (short)(((long)v * q + 8192) >> 14); }

void sim_init(Sim *S, short arena, uint32_t seed)
{
    short x, y;
    const unsigned char *m = ARENA_MAP[arena];
    for (y = 0; y < TH; y++)
        for (x = 0; x < 32; x++)
            S->map32[y][x] = x < TW ? (S->map[y][x] = m[y * TW + x]) : 1;
    for (x = 0; x < MAXT; x++)
        S->t[x].alive = 0;
    for (x = 0; x < MAXS; x++)
        S->s[x].alive = 0;
    S->nt = 0;
    S->tick = 0;
    S->rng = seed ? seed : 1;
    S->nev = 0;
}

short sim_add_tank(Sim *S, short type, short team, short tx, short ty, unsigned char a)
{
    short i = S->nt++;
    Tank *t = &S->t[i];
    t->alive = 1;
    t->type = type;
    t->team = team;
    t->shield = 0;
    t->x = (short)(tx * TS + TS / 2);
    t->y = (short)(ty * TS + TS / 2);
    t->speed = 0;
    t->cool = 0;
    t->nshells = 0;
    t->by = -1;
    t->btimer = 0;
    t->hull = a;
    t->tur = a;
    t->bhead = a;
    t->pad = 0;
    t->drive = 0;
    t->turn = 0;
    t->tgt = a;
    t->fire = 0;
    return i;
}

static void event(Sim *S, short kind, short x, short y, short a)
{
    if (S->nev < MAXEV) {
        SimEv *e = &S->ev[S->nev++];
        e->kind = kind;
        e->x = x;
        e->y = y;
        e->a = a;
    }
}

static uint32_t rng_next(Sim *S)
{
    uint32_t s = S->rng;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    S->rng = s;
    return s;
}

/* is a tank box at (x, y) blocked by a wall or another live tank? */
static short blocked(const Sim *S, short x, short y, short self)
{
    short x0 = (x - HB) >> 8, x1 = (x + HB - 1) >> 8, y0 = (y - HB) >> 8, y1 = (y + HB - 1) >> 8, j;
    if (S->map32[y0][x0] | S->map32[y0][x1] | S->map32[y1][x0] | S->map32[y1][x1])
        return 1;
    for (j = 0; j < S->nt; j++) {
        const Tank *o = &S->t[j];
        if (j != self && o->alive && iabs(x - o->x) < 2 * HB && iabs(y - o->y) < 2 * HB)
            return 1;
    }
    return 0;
}

static void kill_shell(Sim *S, Shell *s)
{
    s->alive = 0;
    S->t[s->owner].nshells--;
}

static void tank_update(Sim *S, short i)
{
    Tank *t = &S->t[i];
    const short *ty = TYPE_TAB[t->type];
    short target, d, dx, dy;
    if (!t->alive)
        return;
    if (t->shield > 0)
        t->shield--;
    t->hull = (unsigned char)(t->hull + t->turn * ty[TY_TURN]);
    target = t->drive > 0 ? ty[TY_MAXF] : t->drive < 0 ? -ty[TY_MAXR] : 0;
    if (t->speed < target) {
        t->speed += ty[TY_ACC];
        if (t->speed > target)
            t->speed = target;
    } else if (t->speed > target) {
        t->speed -= ty[TY_ACC];
        if (t->speed < target)
            t->speed = target;
    }
    dx = mulq(t->speed, COSQ(t->hull));
    dy = mulq(t->speed, SINQ(t->hull));
    if (dx && !blocked(S, t->x + dx, t->y, i))
        t->x += dx;
    if (dy && !blocked(S, t->x, t->y + dy, i))
        t->y += dy;
    d = (signed char)(t->tgt - t->tur);
    if (d > ty[TY_TRATE])
        t->tur = (unsigned char)(t->tur + ty[TY_TRATE]);
    else if (d < -ty[TY_TRATE])
        t->tur = (unsigned char)(t->tur - ty[TY_TRATE]);
    else
        t->tur = t->tgt;
    if (t->cool > 0)
        t->cool--;
    if (t->fire && t->cool == 0 && t->nshells < ty[TY_NMAX]) {
        short sx = t->x + mulq(BARREL, COSQ(t->tur)), sy = t->y + mulq(BARREL, SINQ(t->tur)), k;
        t->cool = ty[TY_COOL];
        if (WALLAT(S, sx, sy)) {
            event(S, EV_DUD, sx, sy, i);
            return;
        }
        for (k = 0; k < MAXS; k++)
            if (!S->s[k].alive)
                break;
        if (k == MAXS)
            return;
        {
            Shell *s = &S->s[k];
            s->alive = 1;
            s->owner = i;
            s->bounces = 0;
            s->age = 0;
            s->x = sx;
            s->y = sy;
            s->vx = mulq(ty[TY_SSPEED], COSQ(t->tur));
            s->vy = mulq(ty[TY_SSPEED], SINQ(t->tur));
            t->nshells++;
            event(S, EV_FIRE, sx, sy, i);
        }
    }
}

static void shells_move(Sim *S)
{
    short k;
    for (k = 0; k < MAXS; k++) {
        Shell *s = &S->s[k];
        short n, hit = 0;
        if (!s->alive)
            continue;
        if (++s->age > SHELL_LIFE) {
            kill_shell(S, s);
            event(S, EV_SHELL_END, s->x, s->y, k);
            continue;
        }
        n = s->x + s->vx;
        if (WALLAT(S, n, s->y)) {
            s->vx = -s->vx;
            hit = 1;
        } else
            s->x = n;
        n = s->y + s->vy;
        if (WALLAT(S, s->x, n)) {
            s->vy = -s->vy;
            hit = 1;
        } else
            s->y = n;
        if (hit) {
            if (++s->bounces > 1) {
                kill_shell(S, s);
                event(S, EV_SHELL_END, s->x, s->y, k);
            } else
                event(S, EV_RICOCHET, s->x, s->y, k);
        }
    }
}

static void shells_vs_shells(Sim *S)
{
    short a, b;
    for (a = 0; a < MAXS; a++) {
        Shell *p = &S->s[a];
        if (!p->alive)
            continue;
        for (b = a + 1; b < MAXS; b++) {
            Shell *q = &S->s[b];
            if (q->alive && iabs(p->x - q->x) < SHELL_HIT && iabs(p->y - q->y) < SHELL_HIT) {
                kill_shell(S, p);
                kill_shell(S, q);
                event(S, EV_SHELLS, (p->x + q->x) >> 1, (p->y + q->y) >> 1, a);
                break;
            }
        }
    }
}

static void shells_vs_tanks(Sim *S)
{
    short k, i;
    for (k = 0; k < MAXS; k++) {
        Shell *s = &S->s[k];
        if (!s->alive)
            continue;
        for (i = 0; i < S->nt; i++) {
            Tank *t = &S->t[i];
            if (!t->alive || iabs(s->x - t->x) >= HIT || iabs(s->y - t->y) >= HIT)
                continue;
            if (i == s->owner && s->bounces == 0)
                continue;
            kill_shell(S, s);
            if (t->shield > 0)
                event(S, EV_SHIELD, s->x, s->y, i);
            else if (i != s->owner && t->team == S->t[s->owner].team)
                event(S, EV_CLANG, s->x, s->y, i);
            else {
                t->alive = 0;
                t->by = s->owner;
                event(S, EV_KILL, t->x, t->y, i);
            }
            break;
        }
    }
}

void sim_tick(Sim *S)
{
    short i;
    S->nev = 0;
    for (i = 0; i < S->nt; i++)
        tank_update(S, i);
    shells_move(S);
    shells_vs_shells(S);
    shells_vs_tanks(S);
    S->tick++;
}

/* ------------------------------------------------------------------ sensing */

unsigned char sim_atan2(short dy, short dx)
{
    short ax = iabs(dx), ay = iabs(dy), r;
    if (!ax && !ay)
        return 0;
    if (ax >= ay)
        r = ATAN_TAB[(unsigned short)((unsigned short)(ay << 6) / (unsigned short)ax)];
    else
        r = 64 - ATAN_TAB[(unsigned short)((unsigned short)(ax << 6) / (unsigned short)ay)];
    if (dx >= 0)
        return (unsigned char)(dy >= 0 ? r : 256 - r);
    return (unsigned char)(dy >= 0 ? 128 - r : 128 + r);
}

short sim_ray(const Sim *S, short x, short y, unsigned char a)
{
    short dx = mulq(RAYSTEP, COSQ(a)), dy = mulq(RAYSTEP, SINQ(a)), n;
    for (n = 1; n <= RAYN; n++) {
        x += dx;
        y += dy;
        if (WALLAT(S, x, y))
            return n;
    }
    return RAYN + 1;
}

short sim_los(const Sim *S, short ax, short ay, short bx, short by)
{
    short dx = bx - ax, dy = by - ay, adx = iabs(dx), ady = iabs(dy), n, k;
    long sx, sy, px, py;
    n = ((adx > ady ? adx : ady) >> 7) + 1;
    sx = ((long)adx << 4) / n;
    sy = ((long)ady << 4) / n;
    if (dx < 0)
        sx = -sx;
    if (dy < 0)
        sy = -sy;
    px = (long)ax << 4;
    py = (long)ay << 4;
    for (k = 1; k < n; k++) {
        px += sx;
        py += sy;
        if (((const unsigned char *)S->map32)[((short)(py >> 7) & ~31) + (short)(px >> 12)])
            return 0;
    }
    return 1;
}

short sim_aim(const Sim *S, short i, short j, short *hit, short *self, short *pts)
{
    const Tank *me = &S->t[i], *T = &S->t[j];
    unsigned char a = me->tur;
    short x = me->x + mulq(BARREL, COSQ(a)), y = me->y + mulq(BARREL, SINQ(a));
    short vx = mulq(AIMSTEP, COSQ(a)), vy = mulq(AIMSTEP, SINQ(a)), b = 0, n, np = 0;
    *hit = 0;
    *self = 0;
    if (WALLAT(S, x, y))
        return 0;
    if (pts) {
        pts[0] = x;
        pts[1] = y;
        np = 1;
    }
    for (n = 0; n < AIMN; n++) {
        short h = 0, m;
        m = x + vx;
        if (WALLAT(S, m, y)) {
            vx = -vx;
            h = 1;
        } else
            x = m;
        m = y + vy;
        if (WALLAT(S, x, m)) {
            vy = -vy;
            h = 1;
        } else
            y = m;
        if (pts) {
            pts[2 * np] = x;
            pts[2 * np + 1] = y;
            np++;
        }
        if (h && ++b > 1)
            break;
        if (iabs(x - T->x) < AIMHIT && iabs(y - T->y) < AIMHIT) {
            *hit = b ? 64 : 127;
            break;
        }
        if (b && iabs(x - me->x) < AIMHIT && iabs(y - me->y) < AIMHIT) {
            *self = 127;
            break;
        }
    }
    return np;
}

short sim_target(const Sim *S, short i)
{
    const Tank *me = &S->t[i];
    short j, best = -1, bd = 32767;
    for (j = 0; j < S->nt; j++) {
        const Tank *o = &S->t[j];
        short dx, dy, d;
        if (j == i || !o->alive || o->team == me->team)
            continue;
        dx = iabs((o->x - me->x) >> 4);
        dy = iabs((o->y - me->y) >> 4);
        d = dx > dy ? dx : dy;
        if (d < bd) {
            bd = d;
            best = j;
        }
    }
    return best;
}

/* vector (dx, dy) in the hull frame: forward and sideways (right) components */
#define ROTF(dx, dy) ((short)(((long)(dx) * C + (long)(dy) * Sn) >> 14))
#define ROTS(dx, dy) ((short)(((long)(dy) * C - (long)(dx) * Sn) >> 14))

void sim_obs(const Sim *S, short i, short *o)
{
    const Tank *me = &S->t[i];
    const short *ty = TYPE_TAB[me->type];
    unsigned char h = me->hull, r;
    short C = COSQ(h), Sn = SINQ(h), k, j, best, bd;
    for (k = 0; k < NRAY; k++)
        o[k] = sim_ray(S, me->x, me->y, (unsigned char)(h + 32 * k)) * 6;
    o[8] = me->speed * 5;
    r = (unsigned char)(me->tur - h);
    o[9] = clamp127(SINQ(r) >> 7);
    o[10] = clamp127(COSQ(r) >> 7);
    o[11] = me->cool * 4 > 127 ? 127 : me->cool * 4;
    o[12] = (ty[TY_NMAX] - me->nshells) * 40;
    o[13] = me->shield > 0 ? 127 : 0;

    best = sim_target(S, i);
    for (k = 14; k < 27; k++)
        o[k] = 0;
    if (best >= 0) {
        const Tank *T = &S->t[best];
        short dxp = (T->x - me->x) >> 4, dyp = (T->y - me->y) >> 4, tvx, tvy, d, hit, self;
        unsigned char at;
        o[14] = clamp127(ROTF(dxp, dyp) >> 2);
        o[15] = clamp127(ROTS(dxp, dyp) >> 2);
        tvx = (short)(((long)T->speed * COSQ(T->hull)) >> 14);
        tvy = (short)(((long)T->speed * SINQ(T->hull)) >> 14);
        o[16] = clamp127((long)ROTF(tvx, tvy) * 5);
        o[17] = clamp127((long)ROTS(tvx, tvy) * 5);
        at = sim_atan2(dyp, dxp);
        r = (unsigned char)(at - me->tur);
        o[18] = clamp127(SINQ(r) >> 7);
        o[19] = clamp127(COSQ(r) >> 7);
        d = iabs(dxp) > iabs(dyp) ? iabs(dxp) : iabs(dyp);
        o[20] = (d >> 2) > 127 ? 127 : (d >> 2);
        o[21] = sim_los(S, me->x, me->y, T->x, T->y) ? 127 : 0;
        sim_aim(S, i, best, &hit, &self, 0);
        o[22] = hit;
        o[23] = self;
        r = (unsigned char)(at + 128 - T->tur);
        o[24] = clamp127(COSQ(r) >> 7);
        o[25] = (T->cool == 0 && T->nshells < TYPE_TAB[T->type][TY_NMAX]) ? 127 : 0;
        o[26] = 127;
    }

    best = -1;
    bd = 32767;
    for (j = 0; j < S->nt; j++) {
        const Tank *a = &S->t[j];
        short dx, dy, d;
        if (j == i || !a->alive || a->team != me->team)
            continue;
        dx = iabs((a->x - me->x) >> 4);
        dy = iabs((a->y - me->y) >> 4);
        d = dx > dy ? dx : dy;
        if (d < bd) {
            bd = d;
            best = j;
        }
    }
    if (best >= 0) {
        short dxp = (S->t[best].x - me->x) >> 4, dyp = (S->t[best].y - me->y) >> 4;
        o[27] = clamp127(ROTF(dxp, dyp) >> 2);
        o[28] = clamp127(ROTS(dxp, dyp) >> 2);
        o[29] = 127;
    } else
        o[27] = o[28] = o[29] = 0;

    {
        short n1 = -1, n2 = -1, d1 = 32767, d2 = 32767;
        for (k = 0; k < MAXS; k++) {
            const Shell *s = &S->s[k];
            short dx, dy, d;
            if (!s->alive || (s->owner == i && s->bounces == 0))
                continue;
            dx = iabs((s->x - me->x) >> 4);
            dy = iabs((s->y - me->y) >> 4);
            d = dx > dy ? dx : dy;
            if (d < d1) {
                d2 = d1;
                n2 = n1;
                d1 = d;
                n1 = k;
            } else if (d < d2) {
                d2 = d;
                n2 = k;
            }
        }
        for (k = 0; k < 2; k++) {
            short n = k ? n2 : n1, *q = o + 30 + 5 * k;
            if (n < 0) {
                q[0] = q[1] = q[2] = q[3] = q[4] = 0;
                continue;
            }
            {
                const Shell *s = &S->s[n];
                short dxp = (s->x - me->x) >> 4, dyp = (s->y - me->y) >> 4;
                q[0] = 127;
                q[1] = clamp127(ROTF(dxp, dyp));
                q[2] = clamp127(ROTS(dxp, dyp));
                q[3] = clamp127(((long)ROTF(s->vx, s->vy) * 3) >> 1);
                q[4] = clamp127(((long)ROTS(s->vx, s->vy) * 3) >> 1);
            }
        }
    }
}

void sim_pick(Sim *S, const int32_t *z, short shift, short *act)
{
    static const short heads[4] = {3, 3, 7, 2};
    short h, k, o = 0, n = 0;
    uint32_t w = 0;
    for (h = 0; h < 4; h++) {
        int32_t best = 0;
        short arg = 0;
        for (k = 0; k < heads[h]; k++) {
            int32_t v;
            if (!(n & 3))
                w = rng_next(S);
            v = z[o + k] + ((int32_t)GUMBEL_Q8[w & 255] << shift);
            w >>= 8;
            n++;
            if (k == 0 || v > best) {
                best = v;
                arg = k;
            }
        }
        act[h] = arg;
        o += heads[h];
    }
}

void sim_apply_action(Sim *S, short i, const short *act)
{
    Tank *t = &S->t[i];
    t->drive = (signed char)(act[0] - 1);
    t->turn = (signed char)(act[1] - 1);
    t->tgt = (unsigned char)(t->tur + TUR_OFF[act[2]]);
    t->fire = (signed char)act[3];
}

/* ------------------------------------------------------------------ scripted bots */

void bot_act(Sim *S, short i, short kind)
{
    Tank *me = &S->t[i];
    short j = sim_target(S, i), dxp, dyp, d, dist, vis, fwd;
    unsigned char at, want;
    if (j < 0) {
        me->drive = 0;
        me->turn = 0;
        me->fire = 0;
        me->tgt = me->tur;
        return;
    }
    dxp = (S->t[j].x - me->x) >> 4;
    dyp = (S->t[j].y - me->y) >> 4;
    dist = iabs(dxp) > iabs(dyp) ? iabs(dxp) : iabs(dyp);
    at = sim_atan2(dyp, dxp);
    me->tgt = at;
    vis = sim_los(S, me->x, me->y, S->t[j].x, S->t[j].y);
    d = (signed char)(at - me->tur);
    me->fire = (signed char)(vis && d >= -4 && d <= 4);
    if (kind == BOT_SITTER) {
        me->drive = 0;
        me->turn = 0;
        return;
    }
    if (kind == BOT_WANDER) {
        if (me->btimer <= 0) {
            uint32_t r = rng_next(S);
            me->bhead = (unsigned char)r;
            me->btimer = (short)(20 + ((r >> 8) & 31));
        } else
            me->btimer--;
        want = me->bhead;
    } else
        want = at;
    fwd = sim_ray(S, me->x, me->y, me->hull);
    if (fwd <= 2) {
        short l = sim_ray(S, me->x, me->y, (unsigned char)(me->hull - 32));
        short r = sim_ray(S, me->x, me->y, (unsigned char)(me->hull + 32));
        me->turn = r >= l ? 1 : -1;
        me->drive = fwd <= 1 ? -1 : 0;
        if (kind == BOT_WANDER)
            me->bhead = (unsigned char)(me->hull + (r >= l ? 64 : -64));
        return;
    }
    d = (signed char)(want - me->hull);
    me->turn = d > 6 ? 1 : d < -6 ? -1 : 0;
    me->drive = (d > -48 && d < 48 && (kind == BOT_WANDER || dist > 64 || !vis)) ? 1 : 0;
}

/* ------------------------------------------------------------------ hash */

static uint32_t mix(uint32_t h, short v)
{
    return ((h << 5) | (h >> 27)) ^ (unsigned short)v;
}

uint32_t sim_hash(const Sim *S)
{
    uint32_t h = (uint32_t)S->tick;
    short i;
    for (i = 0; i < S->nt; i++) {
        const Tank *t = &S->t[i];
        h = mix(h, t->alive);
        h = mix(h, t->x);
        h = mix(h, t->y);
        h = mix(h, t->speed);
        h = mix(h, t->hull);
        h = mix(h, t->tur);
        h = mix(h, t->cool);
        h = mix(h, t->nshells);
        h = mix(h, t->shield);
        h = mix(h, t->by);
    }
    for (i = 0; i < MAXS; i++) {
        const Shell *s = &S->s[i];
        if (!s->alive)
            continue;
        h = mix(h, i);
        h = mix(h, s->x);
        h = mix(h, s->y);
        h = mix(h, s->vx);
        h = mix(h, s->vy);
        h = mix(h, s->bounces);
        h = mix(h, s->age);
        h = mix(h, s->owner);
    }
    h = mix(h, (short)S->rng);
    return h;
}

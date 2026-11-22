/*
 * The attract-mode player: a rule-based flipper controller (no learning needed at this size).
 *
 * Per flipper it watches the balls in a zone above the flipper, measured in the flipper's own
 * frame at rest (t = distance along it from the pivot, h = height above its top face):
 *   - a fast ball coming down: flip as it reaches the chosen aim point t, if it's low enough;
 *   - a slow ball: trap it (hold the flipper up so it rolls into the cradle), wait a moment,
 *     drop the flipper and flip again when the rolling ball reaches the aim point.
 * The aim point decides where the shot goes (late flips go across the table). Plunger: hold
 * for a random time, release.
 */
#include "ai.h"
#include "aimtab.h"            /* generated: aim_points[flipper][target] */

#define MULS(a, b) ((int32_t)(int16_t)(a) * (int16_t)(b))

uint32_t ai_rand(Ai *ai)
{
    ai->rng = ai->rng * 1103515245UL + 12345UL;
    return (ai->rng >> 8) & 0xFFFFFF;
}

void ai_init(Ai *ai, uint32_t seed)
{
    char *p = (char *)ai;
    uint16_t i;
    for (i = 0; i < sizeof(Ai); i++)
        p[i] = 0;
    ai->rng = seed ? seed : 1;
    ai->skill = 3;
    ai->plunge_len = 0;
}

/* where to flip: a point along the flipper (px*64); negative: tap-pass the ball to the other
   flipper at that point, because the target can only be made from there */
static int16_t pick_aim(Ai *ai, int16_t f, const Flipper *fl)
{
    int16_t len = fl->d.len, w = ai->want;
    if (w > 0 && w < AIM_N && (ai_rand(ai) & 7) != 0) {
        if (aim_points[f][w][0])
            return aim_points[f][w][1 + ai_rand(ai) % aim_points[f][w][0]];
        if (aim_points[1 - f][w][0])
            return (int16_t)-(7 * 64 + (int16_t)(ai_rand(ai) % (4 * 64)));   /* 7..11 px: a gentle tap */
    }
    /* anywhere from a quarter to 85% of the way out */
    return (int16_t)(len / 4 + (int32_t)(ai_rand(ai) % 1000) * (len * 6 / 10) / 1000);
}

uint16_t ai_tick(Ai *ai, const World *w)
{
    uint16_t out = 0;
    int16_t f, i;
    const Plunger *pl = &w->plunger;
    for (f = 0; f < w->nflip && f < 2; f++) {
        const Flipper *fl = &w->flip[f];
        int16_t ux = fcos(fl->d.rest), uy = fsin(fl->d.rest);
        int16_t best = -1;
        int16_t bt = 0, bh = 0;
        int32_t bvn = 0, bspeed = 0;
        /* the ball nearest to this flipper inside its zone */
        for (i = 0; i < MAX_BALLS; i++) {
            const Ball *b = &w->ball[i];
            int16_t dx, dy, t, h;
            int32_t vx, vy;
            if (b->state != BALL_FREE || b->layer != 1)
                continue;
            dx = (int16_t)((b->x >> 10) - fl->d.px);
            dy = (int16_t)((b->y >> 10) - fl->d.py);
            t = (int16_t)((MULS(dx, ux) + MULS(dy, uy)) >> 14);
            /* height above the top face: the top normal of a left flipper (dir -1) is (uy,-ux) */
            h = (int16_t)(((MULS(dx, uy) - MULS(dy, ux)) >> 14) * -fl->dir);
            if (t < -6 * 64 || t > fl->d.len + 6 * 64 || h < -2 * 64 || h > 30 * 64)
                continue;
            vx = b->vx >> 4;
            vy = b->vy >> 4;
            if (best < 0 || h < bh) {
                best = i;
                bt = t;
                bh = h;
                bvn = vy;
                bspeed = (vx < 0 ? -vx : vx) + (vy < 0 ? -vy : vy);
            }
        }
        switch (ai->state[f]) {
        case AI_IDLE:
            if (best < 0)
                break;
            if (bspeed > 10400 && bvn > 0) {            /* faster than about 300 px/s */
                /* fast and coming down: flip at the aim point (or trap it sometimes) */
                if (ai->aim[f] <= 0)
                    ai->aim[f] = (int16_t)(fl->d.len / 4 + (int32_t)(ai_rand(ai) % 1000) * (fl->d.len * 6 / 10) / 1000);
                if (bh < 16 * 64 && bt >= ai->aim[f] - 6 * 64) {
                    ai->state[f] = AI_FLIP;
                    ai->timer[f] = 14;
                    ai->aim[f] = 0;
                }
            } else if (bh < 20 * 64 && (int16_t)(ai_rand(ai) % 4) < ai->skill) {
                ai->state[f] = AI_CRADLE;              /* slow ball: trap it */
                ai->timer[f] = (int16_t)(50 + ai_rand(ai) % 70);
            } else if (bh < 10 * 64) {
                if (ai->aim[f] <= 0)
                    ai->aim[f] = (int16_t)(fl->d.len / 4 + (int32_t)(ai_rand(ai) % 1000) * (fl->d.len * 6 / 10) / 1000);
                if (bt >= ai->aim[f]) {
                    ai->state[f] = AI_FLIP;
                    ai->timer[f] = 14;
                    ai->aim[f] = 0;
                }
            }
            break;
        case AI_FLIP:
            out |= (uint16_t)(1 << f);
            if (--ai->timer[f] <= 0)
                ai->state[f] = AI_IDLE;
            break;
        case AI_CRADLE:
            out |= (uint16_t)(1 << f);
            if (best < 0) {                           /* lost it */
                ai->state[f] = AI_IDLE;
                break;
            }
            if (bspeed < 800 && --ai->timer[f] <= 0) {
                ai->state[f] = AI_ROLL;               /* settled: drop and let it roll */
                ai->aim[f] = pick_aim(ai, f, fl);
                ai->timer[f] = 120;
            }
            break;
        case AI_ROLL:
            if (best < 0 || --ai->timer[f] <= 0) {
                ai->state[f] = AI_IDLE;
                break;
            }
            if (ai->aim[f] < 0 && bt >= -ai->aim[f] && bh < 12 * 64) {
                ai->state[f] = AI_FLIP;             /* tap pass: one tick */
                ai->timer[f] = 1;
                ai->aim[f] = 0;
            } else if (ai->aim[f] > 0 && bt >= ai->aim[f] && bh < 12 * 64) {
                ai->state[f] = AI_FLIP;
                ai->timer[f] = 14;
                ai->aim[f] = 0;
            }
            break;
        }
    }
    /* plunger: a ball resting on it -> hold for a while, then let go */
    for (i = 0; i < MAX_BALLS; i++) {
        const Ball *b = &w->ball[i];
        if (b->state == BALL_FREE && (b->x >> 10) > pl->x0 && (b->x >> 10) < pl->x1 &&
            b->y > pl->yrest - (12L << 16) && w->has_plunger)
            break;
    }
    if (i < MAX_BALLS && !pl->firing) {
        if (!ai->plunge_len)
            ai->plunge_len = (int16_t)(30 + ai_rand(ai) % 40);
        if (ai->plunge_t < ai->plunge_len) {
            ai->plunge_t++;
            out |= 4;
        }
    } else {
        ai->plunge_t = 0;
        ai->plunge_len = 0;
    }
    return out;
}

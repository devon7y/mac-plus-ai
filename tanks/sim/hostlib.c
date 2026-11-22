/* Host driver for the C simulation (test_bitexact.py loads it with ctypes).
 *
 * run_game plays one game: tanks from spec (type, team, x SU, y SU, angle), controllers per tank
 * (0 raw inputs from `inp` every tick, 1..3 scripted bot of that kind, 4 policy-style actions from
 * `inp` at the tank's decision ticks), and records the state hash after every tick and every
 * tank's observation before every tick. */
#include <string.h>
#include "sim.h"

static Sim S;
Sim *hostlib_sim(void) { return &S; }

void run_game(short arena, unsigned int seed, short nt, const short *spec, const short *ctrl, long ticks,
              const short *inp, unsigned int *hash, short *obs)
{
    long k;
    short i;
    sim_init(&S, arena, seed);
    for (i = 0; i < nt; i++) {
        short tx = 0, ty = 0;
        sim_add_tank(&S, spec[5 * i], spec[5 * i + 1], tx, ty, (unsigned char)spec[5 * i + 4]);
        S.t[i].x = spec[5 * i + 2];
        S.t[i].y = spec[5 * i + 3];
    }
    for (k = 0; k < ticks; k++) {
        for (i = 0; i < nt; i++) {
            const short *in = inp + (k * nt + i) * 4;
            if (obs)
                sim_obs(&S, i, obs + (k * nt + i) * NOBS);
            if (ctrl[i] == 0) {
                S.t[i].drive = (signed char)in[0];
                S.t[i].turn = (signed char)in[1];
                S.t[i].tgt = (unsigned char)in[2];
                S.t[i].fire = (signed char)in[3];
            } else if (k % DECIDE == i % DECIDE) {
                if (ctrl[i] == 4)
                    sim_apply_action(&S, i, in);
                else
                    bot_act(&S, i, ctrl[i]);
            }
        }
        sim_tick(&S);
        hash[k] = sim_hash(&S);
    }
}

/* sim_pick for n sets of 15 logits from one seed (the rng carries over); out: 4 actions each */
void pick_test(unsigned int seed, long n, const int *z, short shift, short *act, unsigned int *rng_out)
{
    long k;
    sim_init(&S, 0, seed);
    for (k = 0; k < n; k++)
        sim_pick(&S, (const int32_t *)z + 15 * k, shift, act + 4 * k);
    *rng_out = S.rng;
}

void obs_of(short arena, short nt, const short *spec, short *obs)
{
    short i;
    sim_init(&S, arena, 1);
    for (i = 0; i < nt; i++) {
        sim_add_tank(&S, spec[5 * i], spec[5 * i + 1], 0, 0, (unsigned char)spec[5 * i + 4]);
        S.t[i].x = spec[5 * i + 2];
        S.t[i].y = spec[5 * i + 3];
    }
    for (i = 0; i < nt; i++)
        sim_obs(&S, i, obs + i * NOBS);
}

/* state dump after run_game (debugging): per tank 14 values, then per shell 8 values */
static Sim *last;
void dump_state(short *out)
{
    short i;
    (void)last;
    extern Sim *hostlib_sim(void);
    Sim *S = hostlib_sim();
    for (i = 0; i < MAXT; i++) {
        Tank *t = &S->t[i];
        short v[14] = {t->alive, t->type, t->team, t->shield, t->x, t->y, t->speed, t->cool, t->nshells, t->by,
                       t->btimer, t->hull, t->tur, t->bhead};
        memcpy(out + i * 14, v, sizeof v);
    }
    for (i = 0; i < MAXS; i++) {
        Shell *s = &S->s[i];
        short v[8] = {s->alive, s->owner, s->bounces, s->age, s->x, s->y, s->vx, s->vy};
        memcpy(out + MAXT * 14 + i * 8, v, sizeof v);
    }
}

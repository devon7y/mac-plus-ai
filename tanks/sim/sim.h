/* Tank Duel: the deterministic fixed-point game simulation (C version).
 *
 * The same code runs on the Mac (Retro68, 68000) and on the host (bit-exactness tests). Its
 * numpy twin (twin.py) must give identical results; constants and tables come from defs.py
 * through sim_tables.c (gen_tables.py). Units: 16 sub-units (SU) per pixel, 256 SU per tile,
 * angles 0..255 (0 = east, 64 = south), 30 ticks per second.
 *
 * Everything is integer: 16-bit state, 16x16 -> 32-bit products, arithmetic right shifts (floor),
 * and divisions only of non-negative numbers.
 */
#ifndef TANKSIM_H
#define TANKSIM_H

#include <stdint.h>
#include "sim_tables.h"

typedef struct {
    short alive, type, team, shield;
    short x, y;              /* centre, SU */
    short speed;             /* SU per tick along the hull, signed */
    short cool, nshells;     /* ticks until it may fire; live shells it owns */
    short by;                /* who killed it (tank index), -1 */
    short btimer;            /* scripted bots' memory */
    unsigned char hull, tur, bhead, pad;
    /* inputs for the next tick */
    signed char drive, turn; /* -1, 0, 1 */
    unsigned char tgt;       /* turret target angle */
    signed char fire;        /* 0, 1 */
} Tank;

typedef struct {
    short alive, owner, bounces, age;
    short x, y, vx, vy;
} Shell;

typedef struct {
    short kind, x, y, a;
} SimEv;

typedef struct {
    unsigned char map32[TH][32];   /* the walls with rows padded to 32 bytes (cheaper indexing on the 68000) */
    unsigned char map[TH][TW];
    Tank t[MAXT];
    Shell s[MAXS];
    short nt;
    long tick;
    uint32_t rng;
    short nev;
    SimEv ev[MAXEV];
} Sim;

#define COSQ(a) (SIN_TAB[(unsigned char)((a) + 64)])
#define SINQ(a) (SIN_TAB[(unsigned char)(a)])
/* map[y >> 8][x >> 8] for SU coordinates (0 <= x, y), through the 32-byte rows */
#define WALLAT(S, x, y) (((const unsigned char *)(S)->map32)[(((y) >> 3) & ~31) + ((x) >> 8)])

/* reset to an arena with no tanks */
void sim_init(Sim *S, short arena, uint32_t seed);
/* add a tank at tile (tx, ty), facing angle a; returns its index */
short sim_add_tank(Sim *S, short type, short team, short tx, short ty, unsigned char a);
/* one tick, with the inputs in each tank's drive/turn/tgt/fire */
void sim_tick(Sim *S);
/* observation of tank i for the policies: NOBS values in -127..127 */
void sim_obs(const Sim *S, short i, short *o);
/* nearest alive enemy of tank i (Chebyshev distance), or -1 */
short sim_target(const Sim *S, short i);
/* scripted bots: set tank i's inputs */
void bot_act(Sim *S, short i, short kind);
/* pick a policy's action from its 15 integer logits z (heads 3, 3, 7, 2): argmax of z + Gumbel noise
   (GUMBEL_Q8[byte] << shift, shift = the output layer's SH - PICK_SHIFT), bytes from the sim's random
   generator, 4 per 32-bit draw, low byte first */
void sim_pick(Sim *S, const int32_t *z, short shift, short *act);
/* apply a policy's action (drive 0..2, turn 0..2, turret 0..6, fire 0..1) to tank i's inputs */
void sim_apply_action(Sim *S, short i, const short *act);
/* integer helpers shared with the game and the AI view */
unsigned char sim_atan2(short dy, short dx);
short sim_ray(const Sim *S, short x, short y, unsigned char a);
short sim_los(const Sim *S, short ax, short ay, short bx, short by);
/* predicted shell path of tank i towards tank j: *hit 127 direct / 64 after a bounce / 0,
   *self 127 if it comes back to i first. If pts is not NULL, fills up to AIMN+1 path points
   (x, y in SU) and returns how many. */
short sim_aim(const Sim *S, short i, short j, short *hit, short *self, short *pts);
/* hash of the whole state (replay checks) */
uint32_t sim_hash(const Sim *S);

/* integer policy (ternary MLP), generic C reference: see policy.h */

#endif

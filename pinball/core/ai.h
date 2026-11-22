/* Neural Pinball: the attract-mode player (rule-based flipper timing). Portable, deterministic. */
#ifndef AI_H
#define AI_H

#include "phys.h"

enum { AI_IDLE = 0, AI_FLIP, AI_CRADLE, AI_ROLL };
/* what the AI wants to hit (the rules set it; tests/aimtab.c measures where to flip for each) */
enum { AIM_ANY = 0, AIM_RAMP, AIM_ORBIT, AIM_DROP, AIM_SINK, AIM_LANES, AIM_BUMPER, AIM_N };

typedef struct Ai {
    uint32_t rng;
    int16_t state[2];          /* per flipper */
    int16_t timer[2];          /* ticks left in the state */
    int16_t aim[2];            /* flip when the ball is this far along the flipper (px * 64) */
    int16_t plunge_t;          /* ticks the plunger has been held */
    int16_t plunge_len;        /* how long to hold it this time */
    int16_t skill;             /* 0..3: how often it traps and aims (vs. flipping at once) */
    int16_t want;              /* AIM_*: the target it goes for after a cradle */
} Ai;

void ai_init(Ai *ai, uint32_t seed);
/* one tick (60 Hz): reads the world, returns button states (bit 0 left flipper, bit 1 right
   flipper, bit 2 plunger) */
uint16_t ai_tick(Ai *ai, const World *w);
uint32_t ai_rand(Ai *ai);

#endif

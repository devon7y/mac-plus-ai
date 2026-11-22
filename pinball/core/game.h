/* Neural Pinball: rules, scoring, modes and the attract-mode AI (portable, deterministic). */
#ifndef GAME_H
#define GAME_H

#include "phys.h"
#include "ai.h"
#include "table_data.h"

/* buttons (game_step / game_tick input) */
#define BTN_LEFT    0x0001
#define BTN_RIGHT   0x0002
#define BTN_PLUNGER 0x0004
#define BTN_NUDGE   0x0008
#define BTN_START   0x0010

/* game states */
enum { GS_ATTRACT = 0, GS_PLAY, GS_OVER };

/* sound requests (bits, collected per tick) */
#define SND_BUMPER   0x0001
#define SND_SLING    0x0002
#define SND_FLIPPER  0x0004
#define SND_DRAIN    0x0008
#define SND_LANE     0x0010
#define SND_RAMP     0x0020
#define SND_DROP     0x0040
#define SND_JACKPOT  0x0080
#define SND_MULTI    0x0100
#define SND_TILT     0x0200
#define SND_LAUNCH   0x0400
#define SND_SPIN     0x0800
#define SND_SINK     0x1000
#define SND_WALL     0x2000
#define SND_KICK     0x4000
#define SND_FANFARE  0x8000

/* spoken callouts (MacinTalk, when present) */
enum { SAY_NONE = 0, SAY_MULTIBALL, SAY_JACKPOT, SAY_SINGULARITY, SAY_TURING, SAY_PASSED,
       SAY_FAILED, SAY_TILT, SAY_DROPOUT, SAY_BACKPROP, SAY_CONVERGED, SAY_SAVED, SAY_GAMEOVER,
       SAY_TRAINED, SAY_SKILL, SAY_COUNT };

#define MSG_LEN 21

typedef struct Game {
    World w;
    Ai ai;
    uint32_t rng;
    int16_t state;              /* GS_* */
    int16_t ai_playing;         /* the AI holds the controls (attract mode) */
    int32_t score;
    int16_t ball;               /* 1..3 */
    int16_t balls_per_game;
    int16_t in_play;            /* balls on the table (incl. held and on the plunger) */
    uint32_t ticks;             /* game ticks since power-up */
    /* per-ball */
    int16_t epoch;              /* bonus multiplier 1..5 */
    int16_t lanes;              /* bits: T R A I N lit */
    int16_t drops_down;         /* bits */
    int16_t drop_reset_t;
    int16_t ramps, orbits, spins, bumps;
    int16_t save_t;             /* ball save ticks left (0 = off) */
    int16_t save_armed;         /* ball save starts at the first switch */
    int16_t skill_lane;         /* lane lit for the skill shot (-1 none) */
    int16_t skill_t;            /* ticks left to make it after the launch */
    int16_t launched;
    int16_t kickback;           /* lit */
    int16_t lock_lit, locks;
    int16_t multiball;          /* balls in multiball (0 = none) */
    int16_t jackpot_lit, super_t;   /* singularity (super jackpot) timer */
    int16_t jackpots;
    int16_t turing_lit, turing_t, turing_hits, turing_need;   /* Turing Test mode */
    int16_t turing_targets;     /* bits: 1 ramp, 2 orbit, 4 drops */
    int16_t face;               /* FACE_* */
    int16_t face_t;
    int32_t bonus;
    int16_t tilt_meter, tilt_warn, tilted;
    int16_t nudge_cool;
    int16_t held_t[MAX_BALLS];  /* sink hold timers */
    int16_t ball_lock[MAX_BALLS];
    int16_t search_t;
    int16_t spinner_ang, spinner_w; /* spinner animation (angle 0..255, speed) */
    int16_t lamp_blink;         /* frame counter for blinking lamps */
    int16_t end_t;              /* ticks into the end-of-ball sequence */
    int16_t over_t;
    uint16_t prev_buttons;
    /* outputs for the front end */
    uint32_t lamps;             /* bit per LAMP_*, as they should be drawn now */
    uint8_t bump_flash, flash_t[3];
    int16_t sh_kickback, sh_drop0;  /* shape indices */
    uint16_t sounds;            /* SND_* requested this tick */
    int16_t say;                /* SAY_* requested this tick */
    char msg[2][MSG_LEN];       /* DMD lines (top, bottom); "" = show the default */
    int16_t msg_t;              /* ticks left for the message */
    int16_t msg_big;            /* the message replaces the score */
    int16_t new_hiscore;
    uint32_t events_seen;
} Game;

void game_init(Game *g, uint32_t seed);
void game_start(Game *g, int16_t ai);         /* new game (ai = attract mode) */
void game_takeover(Game *g);                  /* a player takes over the AI's game */
void game_step(Game *g, uint16_t buttons);    /* one physics step (STEPS_PER_TICK per tick) */
void game_tick(Game *g, uint16_t buttons);    /* once per tick, after its steps */
uint16_t game_ai_buttons(Game *g);            /* the AI's buttons for this tick */
int16_t game_lamp(const Game *g, int16_t lamp);
void game_message(Game *g, const char *top, const char *bottom, int16_t ticks, int16_t big);
void fmt_score(int32_t v, char *out);         /* "12,345,670" */
void game_debug_multiball(Game *g);           /* tests: start a 3-ball multiball now */

#endif

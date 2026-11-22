/* Orbit Duel: the deterministic game simulation (fixed point, no Toolbox calls).
 *
 * Both Macs of a network game run this same code on the same inputs and must stay bit-identical:
 * integer arithmetic only, our own random generator in the state, no reads of Ticks, the screen
 * or anything else outside the Sim struct. One step = 1/30 s of game time, whatever the frame rate.
 * The struct holds only 16- and 32-bit fields (no padding bytes), so sim_checksum can hash its raw
 * bytes. The file also compiles on the host (tools/simtest.c).
 */
#ifndef SIM_H
#define SIM_H

#include <stdint.h>

#define ARENA_W 448                 /* px; the playfield wraps around at its edges */
#define ARENA_H 336
#define STAR_X (ARENA_W / 2)
#define STAR_Y (ARENA_H / 2)
#define FP 12                       /* positions and velocities: 1 px = 4096 units */
#define SIM_HZ 30                   /* steps per second of game time */

/* Input bits (one byte per player per step) */
#define IN_LEFT   0x01
#define IN_RIGHT  0x02
#define IN_THRUST 0x04
#define IN_FIRE   0x08
#define IN_HYPER  0x10
#define IN_START  0x20              /* pause / resume; also starts the next match */

#define MAX_TORPS 4                 /* per ship, in flight at once */
#define HYPER_T 32                  /* steps spent in hyperspace */
#define HYPER_COOL 60               /* steps after coming back before the next jump */
#define TORP_LIFE 36                /* steps (1.2 s) */

/* phases */
#define PH_PLAY      0
#define PH_ROUND_END 1              /* a ship has exploded; the round ends after a delay */
#define PH_MATCH_END 2              /* someone reached first_to; fire or Return starts a rematch */
#define PH_READY     3              /* a new round: the ships wait a moment */

/* events of the last step (sound and screen effects; OR them over several steps) */
#define EV_FIRE0   0x0001
#define EV_FIRE1   0x0002
#define EV_BOOM    0x0004
#define EV_HYPER   0x0008
#define EV_ROUND   0x0010           /* round over: scores changed */
#define EV_MATCH   0x0020           /* match over */
#define EV_NEWROUND 0x0040
#define EV_PAUSE   0x0080           /* pause state changed */
#define EV_HYPEROUT 0x0100

typedef struct Ship {
    int32_t x, y, vx, vy;           /* FP units */
    int16_t angle;                  /* 0..255, 0 = right, 64 = up (counter-clockwise) */
    int16_t alive;
    int16_t hyper;                  /* > 0: in hyperspace, steps left */
    int16_t hyper_cool;             /* steps until hyperspace can be used again */
    int16_t jumps;                  /* hyperspace jumps this round (each one riskier) */
    int16_t reload;                 /* steps until the next torpedo */
    int16_t dead_t;                 /* steps since the explosion */
    int16_t thrust;                 /* thrusting this step (for the flame) */
    int16_t prev_in;                /* last step's input (edge detection) */
    int16_t cause;                  /* how it died: 1 torpedo, 2 star, 3 collision, 4 hyperspace */
} Ship;

typedef struct Torp {
    int32_t x, y, vx, vy;
    int16_t life;                   /* steps left; 0 = free slot */
    int16_t age;
} Torp;

typedef struct Sim {
    uint32_t rng;
    int32_t tick;                   /* steps simulated so far */
    int16_t phase, phase_t;
    int16_t paused;
    int16_t first_to;
    int16_t score[2];
    int16_t winner;                 /* round or match winner: 0, 1, or 2 = nobody */
    int16_t round;
    int16_t matches;
    int16_t events;
    Ship ship[2];
    Torp torp[2][MAX_TORPS];
} Sim;

void sim_init(Sim *s, uint32_t seed, int16_t first_to);
void sim_step(Sim *s, uint8_t in0, uint8_t in1);
uint32_t sim_checksum(const Sim *s);

/* tables shared with the renderer */
extern const int16_t sin_q14[256];  /* sin(a * 2pi / 256) * 16384 */
#define COS_Q14(a) sin_q14[((a) + 64) & 255]
#define SIN_Q14(a) sin_q14[(a) & 255]
int16_t angle_of(int32_t dx, int32_t dy);   /* screen vector (y down) -> 0..255 */
int32_t isqrt32(uint32_t v);

/* The computer pilot. level 0 easy, 1 normal, 2 hard. auto_restart: press fire after a match
   (attract mode and tests). */
typedef struct Ai {
    uint32_t rng;
    int16_t level, auto_restart;
    int16_t hold, last, wobble;
} Ai;
void ai_init(Ai *ai, uint32_t seed, int16_t level, int16_t auto_restart);
uint8_t ai_input(Ai *ai, const Sim *s, int16_t me);

#endif

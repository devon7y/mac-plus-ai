/* Rail Runner game logic. Mirrors runner/proto/sim.py and game.py exactly (integer maths, same
 * order of operations), so the Python escape-route proof and replays apply to this code. */
#ifndef GAME_H
#define GAME_H
#include "data.h"

enum { K_TRAIN, K_RAMP, K_LOW, K_HIGH, K_WALL, K_ONCOMING };
enum { IN_NONE, IN_LEFT, IN_RIGHT, IN_JUMP, IN_ROLL, IN_BOARD };
enum { EV_OK, EV_CRASH, EV_STUMBLE };
enum { PU_NONE, PU_JETPACK, PU_SNEAKERS, PU_MAGNET, PU_MULT, PU_BOARD, N_PU };
enum { ST_RUN, ST_CRASHED, ST_CAUGHT };

#define MAX_OBJ 48
#define MAX_COIN 112

typedef struct { short kind, lane; long z0, z1; } Obj;
typedef struct { long z; short lane, y, type; } Coin;      /* type 0 = coin, else PU_* */

typedef struct {
    short lane, frm, sw, y, vy, gnd, roll, cl;
} Player;

typedef struct {
    long z;                 /* player's world z (zu) */
    short v;                /* speed, zu per tick */
    long ticks;
    Player p;
    Obj obj[MAX_OBJ];
    short nobj;
    Coin coin[MAX_COIN];
    short ncoin;
    long next_pat;          /* world z of the next pattern's row 0 */
    short last_pat;
    unsigned long rng;
    short state, state_t;   /* ST_*; ticks since entering the state */
    short stumble_t, chase_t, invuln_t, jet_t, sneak_t, magnet_t, mult_t, hover_t, boards;
    long score, coins;
    long last_u;            /* floor(z / U) at the last score update */
    long next_sky;          /* jetpack sky coins: next spawn z */
    short pu_got;           /* last power-up collected (for the HUD flash) */
    short events;           /* bit flags this tick, for sound: 1 coin, 2 jump, 4 roll, 8 crash, 16 stumble, 32 power-up, 64 lane */
} Game;

void game_init(Game *g, unsigned long seed);
void game_tick(Game *g, short input);
short game_collision_lane(const Player *p);

#endif

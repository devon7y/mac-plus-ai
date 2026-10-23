/* Blockworld player movement: walking, collision, gravity, jumping, auto-jump.
 * A port of engine/player.py, bit-identical (tests/check_player.py). Positions 8.8 blocks,
 * velocities 1/256 block per tick, 20 ticks per second. x, z: centre of the feet; y: their bottom. */
#ifndef BW_PLAYER_H
#define BW_PLAYER_H

#define PL_HW 77            /* half width 0.3 blocks */
#define PL_HT 461           /* height 1.8 blocks */
#define PL_EYE 415          /* eye 1.62 blocks above the feet */
#define PL_WALK 55          /* 4.3 blocks/s */
#define PL_GRAV 20
#define PL_JUMP 120         /* jumps 1.17 blocks */
#define PL_VMAX 200
#define PL_PITCH 200        /* pitch limit (rows of horizon shift) */

typedef struct Player {
    long x, y, z;
    short vy, yaw, pitch, ground;
    long autojumps;
} Player;

void player_init(Player *p, long x, long y, long z, short yaw);
/* turn (yaw += dyaw, 1024 per turn) and look up/down (pitch += dpitch, clamped) */
void player_look(Player *p, short dyaw, short dpitch);
/* one 1/20 s step; fwd, strafe in -1..1 (W/S, D/A), jump = Space held */
void player_tick(Player *p, short fwd, short strafe, short jump);
/* pushed (knockback) by (dx, dz), each axis only if there is room */
void player_push(Player *p, long dx, long dz);
/* is block (x, z, y) solid for movement? */
int player_solid(int x, int z, int y);

#endif

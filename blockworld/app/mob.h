/* Blockworld mobs: four animals and four monsters (original designs; the pictures and the table of
 * kinds come from gen_mobs.py), their bodies, AI, combat, arrows, spawning and drawing. */
#ifndef BW_MOB_H
#define BW_MOB_H

#include "render.h"
#include "player.h"
#include "mobs_gen.h"

#define NMOB 12                 /* alive at once */
#define NPASSIVE 6              /* the caps: animals, monsters */
#define NHOSTILE 4
#define NARROW 10

enum { MO_IDLE, MO_WALK, MO_FLEE, MO_CHASE };

typedef struct Mob {            /* 36 bytes (tests read them) */
    long x, y, z;               /*  0: the middle of its feet (8.8) */
    short vx, vy, vz;           /* 12: knockback, fall (1/256 block per step) */
    short yaw;                  /* 18: facing (1024 a turn; 0 = +x, 256 = +z) */
    unsigned char kind, alive, ground, mode;    /* 20 */
    short hp, timer, hurt, fuse, cool, walk;    /* 24: health, AI timer, hurt flash, fuse or leap,
                                                   attack cooldown, walking frames */
} Mob;

typedef struct Arrow {
    long x, y, z;
    short vx, vy, vz, life;
    unsigned char alive, mob;   /* shot by a mob (hurts the player) or by the player (hurts mobs) */
} Arrow;

extern Mob mobs[NMOB];
extern Arrow arrows[NARROW];

/* the game's side of things */
typedef struct MobWorld {
    short night;                                        /* hostile mobs spawn under the sky; day burns some */
    short spawn;                                        /* natural spawning on */
    void (*hurt_player)(short dmg, long fromx, long fromz);
    void (*explode)(long x, long y, long z);            /* a sizzler went off there */
    void (*drop)(unsigned char item, short n, long x, long y, long z);   /* a kill's drop, there */
    unsigned long kills;
} MobWorld;

/* one 1/20 s step of every mob and arrow, and spawning; returns 1 if anything near moved */
short mobs_tick(const Player *pl, MobWorld *w);
/* every mob and arrow within the view distance, depth-tested against the runs */
void mobs_draw(const RCam *cam, unsigned char *buf, short fw, short fh, short moving, short dmax);
/* the mob under the crosshair (the frame's centre) nearer than maxd (8.8 depth): index or -1;
   *depth gets its depth */
short mob_pick(const RCam *cam, short fw, short fh, long maxd, long *depth);
/* hit mob i for dmg, knocked along direction yaw */
void mob_hurt(short i, short dmg, short yaw, MobWorld *w);
short mob_spawn(short kind, long x, long y, long z);      /* index or -1 (no room) */
void arrow_shoot(long x, long y, long z, short vx, short vy, short vz, unsigned char mob);
short mobs_hostile_near(long x, long z, long r);          /* monsters within r (8.8) */
short mob_count(short hostile);
short yaw_to(long dx, long dz);                           /* the yaw facing along (dx, dz) */

#endif

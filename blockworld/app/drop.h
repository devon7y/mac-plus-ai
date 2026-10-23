/* Blockworld dropped items: what mining, objects and mobs give lies on the ground as a small
 * picture of the item until the player walks over it (items within 2 blocks drift to the player). */
#ifndef BW_DROP_H
#define BW_DROP_H

#include "render.h"
#include "player.h"

#define NDROP 48

typedef struct Drop {              /* 16 bytes (saved, tests read them) */
    long x, y, z;                  /*  0: 8.8, the bottom middle */
    short vy;                      /* 12 */
    unsigned char item, count;     /* 14 (count 0: free) */
} Drop;

extern Drop drops[NDROP];
extern unsigned short drop_age[NDROP];

void drop_spawn(unsigned char item, short count, long x, long y, long z);
/* one 1/20 s step: falling, drifting to the player, pick-up (all at once if collect_all);
   returns 1 if one near the player moved or went, | 2 if the inventory changed */
short drops_tick(const Player *pl, short collect_all);
void drops_draw(const RCam *cam, unsigned char *buf, short fw, short fh, short moving, short dmax);

#endif

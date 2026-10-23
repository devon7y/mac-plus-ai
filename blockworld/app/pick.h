/* Blockworld: the block under the crosshair, and its outline on the frame. */
#ifndef BW_PICK_H
#define BW_PICK_H

#include "render.h"

#define PICK_REACH (9 * 128)    /* 4.5 blocks (8.8) */

/* faces: the side of the hit block the ray came in through */
enum { F_XN, F_XP, F_YN, F_YP, F_ZN, F_ZP };

typedef struct Pick {
    short hit;                  /* 0: nothing within reach */
    short x, y, z;              /* the block */
    short face;                 /* F_*: its face the ray entered */
    short ax, ay, az;           /* the cell in front of that face (where a block would be placed) */
    long t;                     /* the ray's length to that face (8.8; PICK_REACH when nothing) */
} Pick;

/* The aim's ray: from the eye (8.8), yaw (1024 per turn), at angle aim above the horizontal
   (1024 per turn, + up; it may look further up or down than the y-sheared view shows).
   solid(x, y, z) says what stops it; the reach is PICK_REACH along the ray. */
void pick_ray(const RCam *cam, short aim, Pick *r, int (*solid)(int x, int y, int z));

/* XOR the outline of the picked face onto the column-pair frame buffer (w x h, focal length
   256 px, horizon h / 2 + pitch). */
void pick_outline(const RCam *cam, const Pick *r, unsigned char *buf, short w, short h);

#endif

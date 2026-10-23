/* Blockworld: game objects that are not cubes (torches, plants, open doors), drawn as sprites
 * against the renderer's per-run depths. The block where an object stands stays air. */
#ifndef BW_OBJ_H
#define BW_OBJ_H

#include "render.h"

#define NOBJ 256
enum { O_NONE, O_TORCH, O_PLANT, O_DOOR };   /* O_PLANT state: 0-3 sapling, 16-23 crop stages */

typedef struct Obj {
    short x, y, z;
    unsigned char type, state;
} Obj;

extern Obj objs[NOBJ];

short obj_at(short x, short y, short z);         /* index, or -1 */
short obj_add(short x, short y, short z, unsigned char type, unsigned char state);
void obj_remove(short i);
void obj_reindex(void);                          /* after objs[] was loaded */

/* A sprite: a picture (pw x ph characters: '#' black, 'o' white, '.' clear) standing at world
   point (x, y, z) (8.8: the middle of its bottom), size sw x sh (8.8 blocks). Drawn into the column-pair frame (fw x fh pixels) where it is
   nearer than the runs there; moving frames use the even columns (8 px wide). */
typedef struct Sprite {
    long x, y, z;
    short sw, sh;
    const char *const *pic;
    short pw, ph;
    short flip;                     /* mirrored left to right */
    short solid;                    /* every texel black (a hurt flash) */
    const unsigned char *dim;       /* white texels dithered with this 8-row pattern (dark), or 0 */
} Sprite;
void sprite_draw(const RCam *cam, const Sprite *s, unsigned char *buf, short fw, short fh, short moving);
/* every object within the view distance */
void objs_draw(const RCam *cam, unsigned char *buf, short fw, short fh, short moving, short dmax);

#endif

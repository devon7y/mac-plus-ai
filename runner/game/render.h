#ifndef RENDER_H
#define RENDER_H
#include "game.h"

typedef struct {
    unsigned short *base;   /* first word of the surface */
    short rw;               /* row stride in words */
    short w, h;             /* pixels */
} Surface;

extern Surface pf;          /* the offscreen playfield */

void render_init(void);
void render_background(long zcam);
void render_objects(const Game *g);
void render_player(const Game *g);
void render_frame(const Game *g);
void blit_to_screen(unsigned char *dst, short dst_rb);
void draw_sprite(const Surface *s, short id, short x, short y);
void fill_span(const Surface *s, short y, short x0, short x1, short pat);
void fill_rect(const Surface *s, short x0, short y0, short x1, short y1, short pat);
void render_invalidate(void);   /* next frame copies every row */
void draw_number(const Surface *s, short x, short y, long v, short min_digits);

#endif

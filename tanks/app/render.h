/* Tank Duel renderer: the arena in an offscreen 1-bit buffer, sprites with masks, and only the
 * changed rectangles copied to the screen (the whole arena in "Show the AI" mode). */
#ifndef RENDER_H
#define RENDER_H

#include <Quickdraw.h>
#include "art.h"

#define AW 384             /* arena, px */
#define AH 288
#define FBRB 52            /* offscreen row bytes: 48 + 4 so a long write at the right edge stays in the row */
#define HUD_H 18

extern unsigned char *r_fb, *r_bg;       /* frame and background buffers, AH rows of FBRB bytes */
extern unsigned char *r_screen;          /* screen address of the arena's top-left pixel */
extern short r_srb;                      /* screen row bytes */
extern Rect r_arena_rect, r_hud_rect;    /* global coordinates */
extern GrafPort r_port;                  /* a QuickDraw port drawing into r_fb */

short r_init(short top, short bottom);   /* place the view between top and bottom; 0 or -1 */
void r_draw_arena(const unsigned char map[18][24]);
/* objects with their own dirty rectangle: tanks 0-3, shells 4-19, effects 20-39, crosshair 40,
   banner 41, AI view lines 42 */
#define R_NOBJ 44
#define OBJ_SHELL 4
#define OBJ_FX 20
#define OBJ_CROSS 40
#define OBJ_BANNER 41
#define OBJ_LINES 42
void r_begin(short full);                /* start a frame: restore last frame's rectangles (or all) */
void r_obj(short id);                    /* what follows belongs to object id */
void r_spr(short idx, short cx, short cy);
void r_dirty(short x0, short y0, short x1, short y1);  /* mark pixels [x0,x1) x [y0,y1) as changed */
void r_line(short x0, short y0, short x1, short y1, short dash);   /* black line (AI view: full frames only) */
void r_box(short x0, short y0, short x1, short y1);    /* white box, black frame (banners) */
void r_logo(short x, short y);           /* title logo, x multiple of 8 */
void r_commit_bg(void);                  /* make the frame buffer the new background (title text) */
void r_present(void);                    /* copy what changed to the screen */
void r_present_all(void);
#endif

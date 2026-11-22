/* Orbit Duel renderer: three 1-bit offscreen buffers and dirty rectangles.
 *
 *   stars  the empty starfield (never changes during a game)
 *   bg     stars + text (scores, messages, title); changed rarely, then marked dirty
 *   fb     bg + moving objects, composed each frame and copied to the screen rect by rect
 *
 * Each buffer is the 448x336 arena with a 16 px margin all round (sprites near an edge are drawn
 * twice, once on each side, and the margin catches what hangs over). Coordinates are arena
 * pixels. 1 = black, as on the Mac screen: space is black, ships and stars are white.
 */
#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>
#include "sim.h"

void r_init(void);                          /* after InitGraf; allocates the buffers */
void r_set_top_limit(int16_t screen_row);   /* don't present above this screen row (menu bar) */
void r_stars(uint32_t seed);                /* new starfield; bg = stars */
void r_bg_clear(int16_t x0, int16_t y0, int16_t x1, int16_t y1);   /* bg region back to stars */
void r_bg_black(int16_t x0, int16_t y0, int16_t x1, int16_t y1);   /* bg region all black */
void r_bg_frame(int16_t x0, int16_t y0, int16_t x1, int16_t y1);   /* white 1 px rectangle in bg */
void r_bg_invert(int16_t x0, int16_t y0, int16_t x1, int16_t y1);
/* vector text into bg (white); returns the width. scale = px per font unit (glyph 4x6 units). */
int16_t r_text(const char *s, int16_t x, int16_t y, int16_t scale, int16_t thick);
int16_t r_text_w(const char *s, int16_t scale);
void r_text_c(const char *s, int16_t y, int16_t scale, int16_t thick);   /* centred */
/* QuickDraw (Chicago 12, white on black) into bg: r_qd_begin() ... DrawString ... r_qd_end() */
void r_qd_begin(void);
void r_qd_end(void);
void r_bg_dirty(int16_t x0, int16_t y0, int16_t x1, int16_t y1);   /* bg changed: show it */
void r_full(void);                          /* black screen around the arena, whole arena from bg */

/* one frame: r_begin, draw calls, r_end */
void r_begin(void);
void r_world(const Sim *s, int16_t frame);  /* star, torpedoes, ships, explosions */
void r_end(void);
void r_glow(int16_t frame);                 /* just the star (link screens) */

extern int16_t r_view_left, r_view_top;     /* arena position on the screen (global coords) */
extern long r_rects_last;                   /* rectangles presented in the last frame */

#endif

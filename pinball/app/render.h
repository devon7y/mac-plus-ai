/* Neural Pinball renderer: table (dirty rectangles over a background with XOR lamp patches),
   sprites, and the side panel with the score display. */
#ifndef RENDER_H
#define RENDER_H

#include "game.h"

void render_init(void);
void render_full(const Game *g);        /* redraw the whole screen (start, update events) */
void render_frame(const Game *g);       /* lamps, sprites, dirty rectangles -> screen */
void panel_update(const Game *g, const char *dmd_top, const char *dmd_mid, const char *dmd_low,
                  int16_t big);         /* the score display; big: mid line in large dots */
void panel_info(const char *const *lines, int16_t n, int16_t face);   /* info plate text */
void panel_present(void);               /* copy changed panel rows to the screen */
long render_stats(void);                /* bytes copied to the screen (for tests) */
extern short comp_x;                    /* screen x of the table's left edge */
extern short scr_w, scr_h;

#endif

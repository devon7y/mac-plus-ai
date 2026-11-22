/* Orbit Duel: playing against another Mac (link.c). */
#ifndef LINK_H
#define LINK_H
enum { LINK_ATALK = 1, LINK_SERIAL = 2 };
void link_init(void);
void link_close(void);                  /* close the network (back on the title) */
void link_screen(short which);          /* enter the AppleTalk chooser or the serial cable screen */
void link_frame(void);                  /* one frame of that screen */
short link_game_steps(short *events);   /* network game: step the sim as far as both inputs allow */
void link_idle(void);                   /* a frame with no step (waiting for the other Mac) */
void link_leave(void);                  /* Esc in a network game: tell the other Mac */
#endif

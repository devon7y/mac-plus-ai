#ifndef SOUND_H
#define SOUND_H
#include "game.h"
void sound_init(void);
void sound_events(short events);   /* called after every game tick with game.events */
void sound_update(const Game *g);  /* called once per frame */
void sound_tick(void);             /* advance the playing effect (idle loops) */
void sound_title(void);            /* title jingle */
void sound_stop(void);             /* silence */
void sound_off(void);              /* silence and disable the sound output (quitting) */
void sound_toggle(void);           /* S key */
short sound_muted(void);
#endif

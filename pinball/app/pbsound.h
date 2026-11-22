#ifndef PB_SOUND_H
#define PB_SOUND_H
#include "game.h"
void sound_init(void);
void sound_events(unsigned short ev);   /* SND_* bits from the game */
void sound_start_tune(void);
void sound_tick(void);                   /* once per tick */
void sound_stop(void);
void sound_reset(void);                  /* after MacinTalk has used the sound buffer */
void sound_off(void);                    /* silence and disable (on quit) */
void sound_set(short on);
short sound_is_on(void);
#endif

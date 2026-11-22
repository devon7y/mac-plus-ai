/* Orbit Duel sound effects (see sound.c). */
#ifndef SOUND_H
#define SOUND_H
enum { SND_FIRE = 1, SND_BOOM, SND_HYPER, SND_BACK, SND_ROUND, SND_MATCH, SND_LINK, SND_CLICK };
void sound_init(void);
void sound_tick(void);              /* once per frame (advances effects, the engine hum) */
void sound_event(short ev);
void sound_engine(short on);        /* the local ship's engine noise */
void sound_stop(void);
void sound_off(void);               /* before quitting */
void sound_set_muted(short m);
short sound_muted(void);
#endif

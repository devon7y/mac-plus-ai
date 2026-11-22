#ifndef TD_SOUND_H
#define TD_SOUND_H
enum { SND_FIRE = 1, SND_EFIRE, SND_RICOCHET, SND_BOOM, SND_CLANG, SND_SHELLS, SND_DUD, SND_PUFF,
       SND_START, SND_CLEAR, SND_OVER, SND_RESPAWN };
void sound_init(void);
void sound_play(short what);
void sound_engine(short on);   /* the player's engine hum while it moves */
void sound_tick(void);         /* once per frame */
void sound_stop(void);
void sound_off(void);          /* silence and disable the sound output (quitting) */
void sound_set(short on);
short sound_on(void);
#endif

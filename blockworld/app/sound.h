/* Blockworld sound effects (a VBL task plays them; see sound.c). Higher ids rank higher. */
#ifndef BW_SOUND_H
#define BW_SOUND_H
enum { FX_DIG, FX_PLACE, FX_CRAFT, FX_HISS, FX_EAT, FX_BOW, FX_BREAK, FX_HIT, FX_DIE, FX_HURT, FX_BOOM, FX_N };
extern short snd_on;
void snd_init(void);
void snd_play(short id);
void snd_toggle(void);             /* M */
void snd_quit(void);               /* before quitting: removes the VBL task */
#endif

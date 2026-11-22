/* MacinTalk callouts ("Multiball!") when MacinTalk is installed; silent otherwise. */
#ifndef PB_SPEECH_H
#define PB_SPEECH_H
short speech_init(void);        /* 1 if MacinTalk is available */
void speech_say(short id);      /* SAY_* from game.h */
void speech_off(void);
extern short speech_available, speech_on;
#endif

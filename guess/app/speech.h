/* Guess My Doodle speaks (copied from llm/app/speech.c): MacinTalk 1.x when its file is in the System Folder (or next to the app). */
#ifndef GUESS_SPEECH_H
#define GUESS_SPEECH_H
#include <MacTypes.h>

typedef struct {            /* in the debug block (main.c, offset 240), read by the MAME test scripts */
    long state;             /* +0  0 not tried, 1 ready, -1 no MacinTalk (file or driver), -3 no memory, else open error */
    long on;                /* +4  "Speak Guesses" */
    unsigned long ticks;    /* +8  ticks spent speaking (Reader + MacinTalk + beeps) */
    unsigned long rdTicks;  /* +12 test build: of which in the Reader (English to phonemes) */
    long count, beeps, err; /* +16 sentences spoken, +20 test build: real beeps, +24 last Reader/MacinTalk error */
    char tx[64], ph[64];    /* +28, +92 test build: the last text given to the Reader and its phonemes */
    long pb[16];            /* the driver's parameter block (it uses offsets 16-63) */
    long dce[6];            /* stand-in DCE: dCtlStorage is dce[5] (offset 20) */
    Handle drvr, reader, phon;
    short mtRef, sysVol, sentLen;
    char sent[256];         /* the reply's current sentence */
} Speech;

/* Open MacinTalk from the System Folder, else from appVol (the app's folder). 1 if ready. Nothing is
   looked for when the app's disk was ejected (the Plus floppies: their System Folder is on it). */
int spInit(Speech *g, short appVol, int appOnline);
int spPref(Speech *g, int set);         /* set < 0: read "Guess Prefs" (1 speak, the default; 0 silent) */
void spSay(Speech *g, const char *s, int n);   /* one sentence (blocks while talking); "Beep!" beeps */
/* One token of a reply (s = NULL: the reply ended), if speaking is on. Each sentence is spoken as soon as it ends: Pixel
   writes about one token every 5 s on a real Plus, so waiting for the whole reply would be slow. */
void spTok(Speech *g, const char *s, int m);

#endif

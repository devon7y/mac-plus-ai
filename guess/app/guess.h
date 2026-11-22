/* Guess My Doodle: shared declarations. */
#ifndef GUESS_H
#define GUESS_H
#include <Multiverse.h>      /* every Toolbox header of the Multiversal Interfaces */
#include <string.h>

#define TICKS (*(volatile unsigned long *)0x16A)

extern short gSoundOn;

/* The debug block that the MAME tests find by its tag (scanned in 2-byte steps). Fixed layout. */
struct DebugBlock {
    unsigned long magic;        /*  0 'GSDB' */
    unsigned long version;      /*  4 1 */
    unsigned long notMagic;     /*  8 ~'GSDB' */
    unsigned long state;        /* 12 0 starting, 1 title, 2 round card, 3 drawing, 4 round over, 5 end, 9 error */
    long round;                 /* 16 0..5 */
    long target;                /* 20 category of this round */
    unsigned long ninfer;       /* 24 guesses made (inferences finished) */
    long top1;                  /* 28 the current top guess (-1 none) */
    long top1pct;               /* 32 its confidence, % */
    long score;                 /* 36 rounds guessed */
    Rect canvas;                /* 40 the drawing area, global coordinates */
    unsigned long inferTicks;   /* 48 ticks from the start to the end of the last guess */
    unsigned long freeMem;      /* 52 FreeMem after loading */
    long modelErr;              /* 56 0 = loaded */
    long speech;                /* 60 MacinTalk: 1 ready, else not */
    unsigned long cmd;          /* 64 test mailbox: 1 = next game's first target is arg */
    long arg;                   /* 68 */
    unsigned long cmdDone;      /* 72 */
    long npts;                  /* 76 points in the drawing */
    long loadTicks;             /* 80 ticks to read and compile the model */
    long okMask;                /* 84 bit r set: round r was guessed */
    long top[3];                /* 88 the last top 3 */
    long pct[3];                /* 100 */
    long ncls;                  /* 112 categories */
    unsigned long codeBytes;    /* 116 */
    unsigned long endTicks[4];  /* 120 end screen: header, after the strokes of round 0, all strokes, done */
    long spoken;                /* 136 sentences spoken (MacinTalk) */
    long spokenErr;             /* 140 the last Reader/MacinTalk error */
};
extern struct DebugBlock dbg;

#endif

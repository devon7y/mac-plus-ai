/* High scores and settings: a small file next to the application (the default directory at
   launch). On a locked floppy saving simply fails and the game goes on. */
#include <Files.h>
#include "hiscore.h"

static const unsigned char fname[] = "\pNeural Pinball Scores";

Scores scores;

static void defaults(void)
{
    static const char *const names[NSCORES] = {"NPB", "SGD", "RNN", "CPU", "BIT"};
    static const long pts[NSCORES] = {2500000L, 1500000L, 1000000L, 500000L, 250000L};
    short i, k;
    scores.version = SCORES_VERSION;
    scores.sound = 1;
    scores.speech = 1;
    for (i = 0; i < NSCORES; i++) {
        for (k = 0; k < 3; k++)
            scores.e[i].init[k] = names[i][k];
        scores.e[i].init[3] = 0;
        scores.e[i].score = pts[i];
    }
}

void scores_load(void)
{
    short ref;
    long n = sizeof(Scores);
    defaults();
    if (FSOpen(fname, 0, &ref) != noErr)
        return;
    if (FSRead(ref, &n, &scores) != noErr || n != (long)sizeof(Scores) || scores.version != SCORES_VERSION)
        defaults();
    FSClose(ref);
}

void scores_save(void)
{
    short ref;
    long n = sizeof(Scores);
    OSErr e = FSOpen(fname, 0, &ref);
    if (e == fnfErr) {
        if (Create(fname, 0, 'NPbl', 'NPhs') != noErr)
            return;
        e = FSOpen(fname, 0, &ref);
    }
    if (e != noErr)
        return;
    SetFPos(ref, fsFromStart, 0);
    FSWrite(ref, &n, &scores);
    SetEOF(ref, n);
    FSClose(ref);
    FlushVol(0, 0);
}

short scores_rank(long score)
{
    short i;
    for (i = 0; i < NSCORES; i++)
        if (score > scores.e[i].score)
            return i;
    return -1;
}

void scores_insert(short rank, long score, const char *init)
{
    short i, k;
    if (rank < 0 || rank >= NSCORES)
        return;
    for (i = NSCORES - 1; i > rank; i--)
        scores.e[i] = scores.e[i - 1];
    for (k = 0; k < 3; k++)
        scores.e[rank].init[k] = init[k] ? init[k] : ' ';
    scores.e[rank].init[3] = 0;
    scores.e[rank].score = score;
}

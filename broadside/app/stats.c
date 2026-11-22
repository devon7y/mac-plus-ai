/*
 * Options, the win/loss record and the shot statistics, kept in "Broadside Prefs" in the System
 * Folder (a locked disk just means nothing is remembered).
 */
#include "broadside.h"

#define PREFS_MAGIC 0x42725066UL      /* 'BrPf' */
#define PREFS_VERSION 1

Prefs gPrefs;
static const unsigned char kPrefsName[] = "\pBroadside Prefs";

static short sys_vref(void)
{
    SysEnvRec env;
    if (SysEnvirons(1, &env) == noErr)
        return env.sysVRefNum;
    return 0;
}

static void defaults(void)
{
    memset(&gPrefs, 0, sizeof gPrefs);
    gPrefs.magic = PREFS_MAGIC;
    gPrefs.version = PREFS_VERSION;
    gPrefs.level = AI_NORMAL;
    gPrefs.sound = 1;
    gPrefs.speech = 1;
}

void prefs_load(void)
{
    short ref, vref = sys_vref();
    long n = sizeof gPrefs;
    Prefs p;
    defaults();
    if (FSOpen(kPrefsName, vref, &ref) != noErr)
        return;
    if (FSRead(ref, &n, &p) == noErr && n == (long)sizeof p && p.magic == PREFS_MAGIC
        && p.version == PREFS_VERSION && p.level <= AI_HARD)
        gPrefs = p;
    FSClose(ref);
}

void prefs_save(void)
{
    short ref, vref = sys_vref();
    long n = sizeof gPrefs;
    OSErr e = FSOpen(kPrefsName, vref, &ref);
    if (e == fnfErr) {
        if (Create(kPrefsName, vref, 'BrDs', 'BrPf') != noErr)
            return;
        e = FSOpen(kPrefsName, vref, &ref);
    }
    if (e != noErr)
        return;
    if (SetFPos(ref, fsFromStart, 0) == noErr && FSWrite(ref, &n, &gPrefs) == noErr)
        SetEOF(ref, n);
    FSClose(ref);
    FlushVol(NULL, vref);
}

void stats_record(int won, int myShots, int macShots, int level, const short *first, int nfirst)
{
    int i;
    if (level < 0 || level > 2)
        return;
    gPrefs.st[level].games++;
    if (won) {
        gPrefs.st[level].wins++;
        gPrefs.st[level].myShotsWon += (uint32_t)myShots;
        if (!gPrefs.st[level].best || myShots < gPrefs.st[level].best)
            gPrefs.st[level].best = (uint16_t)myShots;
    } else {
        gPrefs.st[level].losses++;
        gPrefs.st[level].macShotsWon += (uint32_t)macShots;
    }
    /* where I shoot first: the Mac hides its ships elsewhere next time (old games fade) */
    for (i = 0; i < NCELL; i++)
        gPrefs.early[i] = (uint16_t)(gPrefs.early[i] - (gPrefs.early[i] >> 3));
    for (i = 0; i < nfirst; i++)
        if (first[i] >= 0 && first[i] < NCELL && gPrefs.early[first[i]] < 60000)
            gPrefs.early[first[i]] = (uint16_t)(gPrefs.early[first[i]] + 64 - 2 * i);
    prefs_save();
}

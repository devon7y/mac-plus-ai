/*
 * Guess My Doodle speaks through MacinTalk 1.x (this file is llm/app/speech.c, Pixel's, unchanged but for the names) (Apple, 1985-87; the "MacinTalk" file, never shipped in git).
 *
 * MacinTalk's interface (SpeechOn, Reader, MacinTalk, SpeechPitch, SpeechRate) is a glue library that
 * isn't in Inside Macintosh, so this file does the glue's work, worked out from MacinTalk 1.31 itself:
 *   DRVR ".SPEECH" (ID 31)  Open: makes its storage (2,816 B) in dCtlStorage and returns the Reader's
 *                           handle at parameter-block offset 52 (offset 60: an exceptions file name;
 *                           empty here). Control: csCode 2 rate (words/min, default 150), 4 pitch (Hz,
 *                           65-500, default 110), 0 natural/robotic. Prime (_Write): ioBuffer = a
 *                           HANDLE to phonemes, ioReqCount = their length; it talks before returning
 *                           (the CPU fills the sound buffer itself, paced by the tick count; interrupts
 *                           stay on, so keys typed meanwhile are kept).
 *   TALK 1                  the Reader, a Pascal function (text: Ptr; len: LONGINT; out: Handle): OSErr
 *                           that turns English into phonemes with the rules in RULZ 129 (GetResource,
 *                           so the MacinTalk file must be the current resource file).
 * The driver's routines use only dCtlStorage of their DCE, so Pixel calls them directly with a stand-in
 * DCE: nothing goes into the unit table, and quitting needs no clean-up (everything is in Pixel's heap,
 * and the Resource Manager closes the file).
 */
#include <Resources.h>
#include <Files.h>
#include <MacMemory.h>
#include <OSUtils.h>
#include <Events.h>
#include "speech.h"

#define PITCH 130           /* Hz */
#define RATE 160            /* words per minute */

/* long spDrv(void *pb, void *dce, Ptr routine): A0 = pb, A1 = DCE, result in D0 (a word) */
/* long spRead(Ptr text, long len, Handle out, Ptr reader): the Reader, a Pascal function */
long spDrv(void *pb, void *dce, Ptr routine);
long spRead(Ptr text, long len, Handle out, Ptr reader);
asm("   .text\n"
    "   .globl spDrv\n"
    "spDrv:\n"
    "   movem.l %d2-%d7/%a2-%a6,-(%sp)\n"
    "   movem.l 48(%sp),%a0-%a2\n"
    "   jsr (%a2)\n"
    "   ext.l %d0\n"
    "   movem.l (%sp)+,%d2-%d7/%a2-%a6\n"
    "   rts\n"
    "   .globl spRead\n"
    "spRead:\n"
    "   movem.l %d2-%d7/%a2-%a6,-(%sp)\n"
    "   move.l 60(%sp),%a0\n"
    "   clr.w -(%sp)\n"
    "   move.l 50(%sp),-(%sp)\n"
    "   move.l 58(%sp),-(%sp)\n"
    "   move.l 66(%sp),-(%sp)\n"
    "   jsr (%a0)\n"
    "   move.w (%sp)+,%d0\n"
    "   ext.l %d0\n"
    "   movem.l (%sp)+,%d2-%d7/%a2-%a6\n"
    "   rts\n");

static long call(Speech *g, short off)      /* the driver routine at header offset off */
{
    return spDrv(g->pb, g->dce, *g->drvr + *(unsigned short *)(*g->drvr + off));
}

static void control(Speech *g, short code, short value)
{
    ((short *)g->pb)[13] = code;            /* csCode, offset 26 */
    ((short *)g->pb)[14] = value;           /* csParam, offset 28 */
    call(g, 12);
}

int spInit(Speech *g, short appVol, int appOnline)
{
    SysEnvRec env;
    short cur = CurResFile();
    long *d;
    static unsigned char none[1];           /* no exceptions file */
    g->state = -1;
    if (!appOnline) return 0;               /* the boot floppy was ejected for disk 2 */
    SysEnvirons(1, &env);
    g->sysVol = env.sysVRefNum;
    if ((g->mtRef = OpenRFPerm("\pMacinTalk", env.sysVRefNum, fsRdPerm)) == -1
        && (g->mtRef = OpenRFPerm("\pMacinTalk", appVol, fsRdPerm)) == -1) return 0;
    UseResFile(g->mtRef);
    g->drvr = Get1IndResource('DRVR', 1);
    if (g->drvr && (d = (long *)(*g->drvr + 18), d[0] == 0x072E5350L && d[1] == 0x45454348L)) {   /* .SPEECH */
        HLock(g->drvr);                     /* its code runs in place (locked blocks are never purged) */
        g->pb[15] = (long)none;             /* offset 60 */
        g->state = call(g, 8);              /* Open */
        g->reader = (Handle)g->pb[13];      /* offset 52 */
        if (!g->state && !(g->phon = NewHandle(256))) g->state = -3;   /* Open fails without TALK 1 */
        if (!g->state) {
            HLock(g->reader);
            control(g, 4, PITCH);
            control(g, 2, RATE);
            g->state = 1;
        }
    }
    UseResFile(cur);
    return g->state == 1;
}

/* "Pixel Prefs" in the System Folder holds the one setting by being there: there = silent */
int spPref(Speech *g, int set)
{
    static const unsigned char name[] = "\pGuess Prefs";
    ParmBlkPtr p = (ParmBlkPtr)g->pb;       /* the driver's block: ioCompletion (offset 12) stays 0 */
    short ref;
    if (set < 0) {
        if (FSOpen(name, g->sysVol, &ref)) return 1;
        FSClose(ref);
        return 0;
    }
    p->ioParam.ioNamePtr = (StringPtr)name;
    p->ioParam.ioVRefNum = g->sysVol;
    p->ioParam.ioVersNum = 0;
    if (set) PBDeleteSync(p);
    else PBCreateSync(p);
    p->ioParam.ioNamePtr = NULL;
    PBFlushVolSync(p);
    return set;
}

/* ------------------------------------------------------------------ text for the Reader
 * MacinTalk's rules read digits one by one ("one nine eight six"), so numbers of up to four digits
 * become words first; 1100-1999 the way years are said ("nineteen eighty six"). */
static const char numw[] = "zero\0one\0two\0three\0four\0five\0six\0seven\0eight\0nine\0ten\0eleven\0twelve\0"
    "thirteen\0fourteen\0fifteen\0sixteen\0seventeen\0eighteen\0nineteen\0twenty\0thirty\0forty\0fifty\0"
    "sixty\0seventy\0eighty\0ninety\0hundred\0thousand\0oh";

static char *word(char *o, unsigned short i)
{
    const char *p = numw;
    while (i--) while (*p++) { }
    while (*p) *o++ = *p++;
    *o++ = ' ';
    return o;
}

static char *n99(char *o, unsigned short v)
{
    if (v < 20) return word(o, v);
    o = word(o, (unsigned short)(18 + v / 10));
    return v % 10 ? word(o, (unsigned short)(v % 10)) : o;
}

static char *number(char *o, unsigned short v)
{
    unsigned short r = v % 100;
    if (v >= 1100 && v < 2000 && r) {
        o = n99(o, (unsigned short)(v / 100));
        if (r < 10) o = word(o, 30);
        return n99(o, r);
    }
    if (v >= 1000) {
        o = word(o, (unsigned short)(v / 1000));
        o = word(o, 29);
        if (!(v %= 1000)) return o;
    }
    if (v >= 100) {
        o = word(o, (unsigned short)(v / 100));
        o = word(o, 28);
        if (!r) return o;
    }
    return n99(o, r);
}

static int digit(char c) { return (unsigned char)(c - '0') <= 9; }

void spSay(Speech *g, const char *s, int n)
{
    char buf[400], *o = buf;
    const char *e = s + n, *p;
    short err, cur, k = 0, w = 0;
    long t0 = TickCount(), t;
    if (g->state != 1) return;
    /* "Beep!", "Beep beep!": nothing but beeps, so Pixel beeps for real */
    for (p = s; p < e; p++) {
        char c = (char)(*p | 32);
        if (c >= 'a' && c <= 'z') {
            w = 1;
            if (k >= 0) k = c == "beep"[k & 3] ? k + 1 : -1;
        } else if (digit(*p)) w = 1, k = -1;
    }
    if (!w) return;                         /* nothing to say ("!" after "!") */
    if (k > 0 && !(k & 3)) {
        k >>= 2;
#ifdef PIXEL_SELFTEST
        g->beeps += k;
#endif
        while (k--) {
            SysBeep(10);
            Delay(8, &t);
        }
    } else {
        while (s < e && o < buf + sizeof buf - 48) {
            if (digit(*s)) {
                unsigned short v = 0;
                for (p = s; p < e && digit(*p) && p - s < 5; p++) v = (unsigned short)(v * 10 + *p - '0');
                if (p - s < 5) {
                    *o++ = ' ';
                    o = number(o, v);
                    s = p;
                    continue;
                }
                while (s < e && digit(*s)) *o++ = *s++;     /* long: digit by digit */
                continue;
            }
            *o++ = *s++;
        }
#ifdef PIXEL_SELFTEST
        *o = 0;
        BlockMove(buf, g->tx, 63);
#endif
        cur = CurResFile();
        UseResFile(g->mtRef);               /* the Reader loads its rules with GetResource */
        err = (short)spRead(buf, o - buf, g->phon, *g->reader);
        UseResFile(cur);
#ifdef PIXEL_SELFTEST
        g->rdTicks += TickCount() - t0;
#endif
        if (!err) {
#ifdef PIXEL_SELFTEST
            BlockMove(*g->phon, g->ph, 63);     /* ends with '#' */
#endif
            g->pb[8] = (long)g->phon;       /* ioBuffer, offset 32: a handle */
            g->pb[9] = GetHandleSize(g->phon);   /* ioReqCount, offset 36 */
            err = (short)call(g, 10);       /* Prime: talks, then returns */
        }
        g->err = err;
        g->count++;
    }
    g->ticks += TickCount() - t0;
}

void spTok(Speech *g, const char *s, int m)
{
    short n = g->sentLen, end = !s;
    char *d = g->sent + n;
    if (!g->on) return;
    for (; m > 0 && n < 255; m--, n++)
        if ((*d++ = *s++) == '.' || d[-1] == '!' || d[-1] == '?') end = 1;
    g->sentLen = n;
    if (end || n > 200) {
        spSay(g, g->sent, n);
        g->sentLen = 0;
    }
}

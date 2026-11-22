/*
 * Neural Pinball callouts through MacinTalk 1.x, when the "MacinTalk" file is in the System
 * Folder (or next to the app); silent otherwise. MacinTalk is Apple's and never goes into git.
 *
 * The glue is Pixel's (llm/app/speech.c, which documents the interface worked out from
 * MacinTalk 1.31): DRVR ".SPEECH" (ID 31) is called directly with a stand-in DCE; Open returns
 * the Reader (TALK 1); Control csCode 0 sets natural/robotic, 2 the rate, 4 the pitch; Prime
 * talks before it returns. Here the voice is MacinTalk's natural mode, pitched low.
 *
 * Speaking keeps the CPU busy for a second or two, so the game only speaks at big moments, and
 * main.c restarts its clock afterwards (the table simply pauses while the Mac talks).
 */
#include <Resources.h>
#include <Files.h>
#include <MacMemory.h>
#include <OSUtils.h>
#include <Events.h>
#include "speech.h"
#include "game.h"

#define PITCH 92            /* Hz: a low computer voice (MacinTalk's default is 110) */
#define RATE 150            /* words per minute */

short speech_available, speech_on;

static long pb[16];         /* the driver's parameter block (it uses offsets 16-63) */
static long dce[6];         /* stand-in DCE: dCtlStorage is dce[5] (offset 20) */
static Handle drvr, reader, phon;
static short mt_ref;

long pbDrv(void *pb, void *dce, Ptr routine);
long pbRead(Ptr text, long len, Handle out, Ptr reader);
asm("   .text\n"
    "   .globl pbDrv\n"
    "pbDrv:\n"
    "   movem.l %d2-%d7/%a2-%a6,-(%sp)\n"
    "   movem.l 48(%sp),%a0-%a2\n"
    "   jsr (%a2)\n"
    "   ext.l %d0\n"
    "   movem.l (%sp)+,%d2-%d7/%a2-%a6\n"
    "   rts\n"
    "   .globl pbRead\n"
    "pbRead:\n"
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

static long call(short off)
{
    return pbDrv(pb, dce, *drvr + *(unsigned short *)(*drvr + off));
}

static void control(short code, short value)
{
    ((short *)pb)[13] = code;               /* csCode, offset 26 */
    ((short *)pb)[14] = value;              /* csParam, offset 28 */
    call(12);
}

short speech_init(void)
{
    SysEnvRec env;
    short cur = CurResFile(), vol;
    long *d, err;
    static unsigned char none[1];
    speech_available = 0;
    SysEnvirons(1, &env);
    GetVol(NULL, &vol);
    if ((mt_ref = OpenRFPerm("\pMacinTalk", env.sysVRefNum, fsRdPerm)) == -1 &&
        (mt_ref = OpenRFPerm("\pMacinTalk", vol, fsRdPerm)) == -1)
        return 0;
    UseResFile(mt_ref);
    drvr = Get1IndResource('DRVR', 1);
    if (drvr && (d = (long *)(*drvr + 18), d[0] == 0x072E5350L && d[1] == 0x45454348L)) {   /* .SPEECH */
        HLock(drvr);
        pb[15] = (long)none;                /* offset 60: no exceptions file */
        err = call(8);                      /* Open */
        reader = (Handle)pb[13];            /* offset 52 */
        if (!err && (phon = NewHandle(256)) != NULL) {
            HLock(reader);
            control(4, PITCH);              /* natural mode: the robotic monotone was hard to
                                               understand over the game's sounds */
            control(2, RATE);
            speech_available = 1;
        }
    }
    UseResFile(cur);
    return speech_available;
}

static void say_text(const char *s)
{
    short n = 0, cur, err;
    if (!speech_available)
        return;
    while (s[n])
        n++;
    cur = CurResFile();
    UseResFile(mt_ref);                     /* the Reader loads its rules with GetResource */
    err = (short)pbRead((Ptr)s, n, phon, *reader);
    UseResFile(cur);
    if (!err) {
        pb[8] = (long)phon;                 /* ioBuffer, offset 32: a handle */
        pb[9] = GetHandleSize(phon);        /* ioReqCount, offset 36 */
        call(10);                           /* Prime: talks, then returns */
    }
}

void speech_say(short id)
{
    static const char *const text[SAY_COUNT] = {
        0, "Multi ball!", "Jackpot!", "Singularity!", "Turing test.", "Test passed.",
        "Test failed.", "Tilt.", "Ball saved.", "Game over.", "Dropout.", "Back prop.", "Converged.",
        "Epoch complete.", "Skill shot!"};
    if (id > 0 && id < SAY_COUNT && text[id])
        say_text(text[id]);
}

void speech_off(void)
{
}

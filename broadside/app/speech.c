/*
 * The Mac's voice: MacinTalk 1.x, when the "MacinTalk" file is in the System Folder (the website's
 * disk has it; the Plus floppies don't, and the game is silent then).
 *
 * MacinTalk's interface isn't in Inside Macintosh. Worked out from the MacinTalk 1.31 file and
 * the Talking Moose's glue:
 *   - the file holds DRVR 31 ".SPEECH" (the synthesiser), TALK 1 (the Reader: English text to
 *     phonemes, a Pascal function (Ptr text, long length, Handle phonemes): OSErr) and RULZ 129
 *     (the Reader's rules, found through the resource chain);
 *   - opening the driver (_Open on ".SPEECH" with the MacinTalk file on top of the resource
 *     chain) takes an extended parameter block: offset 60 points to the name of an exceptions
 *     file (an empty string: none; "noReader": no Reader), and the driver returns the TALK handle
 *     at offset 52;
 *   - speaking is an immediate _Write ($A203): ioBuffer = the phonemes' Handle, ioReqCount = its
 *     size; it runs synchronously, writing the sound buffer itself;
 *   - immediate _Control ($A204): csCode 2 = rate (words a minute, 85..425, default 150),
 *     csCode 4 = pitch (Hz, 65..500, default 110), csCode 0 = 0 natural / 1 robotic.
 *   - closing the driver frees its storage and its unit-table entry.
 */
#include "broadside.h"

static short gTalkRes = -1;       /* the MacinTalk file */
static short gDrvRef;             /* the .SPEECH driver */
static Handle gReader;            /* TALK 1 */
static short gOk;
static long gOpenPB[24];          /* 96 bytes: an IOParam plus the extension the driver reads */

static short trap_a0(short trap, void *pb)
{
    register void *a0 __asm__("a0") = pb;
    register short d0 __asm__("d0");
    if (trap == 0x203)
        __asm__ volatile(".short 0xA203" : "=r"(d0) : "r"(a0) : "d1", "d2", "a1", "memory", "cc");
    else
        __asm__ volatile(".short 0xA204" : "=r"(d0) : "r"(a0) : "d1", "d2", "a1", "memory", "cc");
    return d0;
}

/* the Reader, a Pascal function: result word, then the arguments left to right */
static short call_reader(void *entry, const char *text, long len, Handle out)
{
    short result;
    __asm__ volatile(
        "clr.w -(%%sp)\n\t"
        "move.l %1,-(%%sp)\n\t"
        "move.l %2,-(%%sp)\n\t"
        "move.l %3,-(%%sp)\n\t"
        "jsr (%4)\n\t"
        "move.w (%%sp)+,%0"
        : "=d"(result)
        : "r"(text), "r"(len), "r"(out), "a"(entry)
        : "d1", "d2", "a0", "a1", "memory", "cc");
    return result;
}

static void control(short code, short value)
{
    CntrlParam pb;
    memset(&pb, 0, sizeof pb);
    pb.ioCRefNum = gDrvRef;
    pb.csCode = code;
    pb.csParam[0] = value;
    trap_a0(0x204, &pb);
}

short speech_init(void)
{
    SysEnvRec env;
    short vref = 0, save = CurResFile(), units = *(short *)0x1D2;
    Handle *utable = *(Handle **)0x11C;
    static const unsigned char empty[2] = {0, 0};
    IOParam *pb = (IOParam *)gOpenPB;
    gOk = 0;
    if (SysEnvirons(1, &env) == noErr)
        vref = env.sysVRefNum;
    gTalkRes = OpenRFPerm("\pMacinTalk", vref, fsRdPerm);
    if (gTalkRes == -1)
        return 0;
    if (units <= 31 || utable[31]) {        /* the driver's unit (its resource ID) is taken */
        CloseResFile(gTalkRes);
        gTalkRes = -1;
        UseResFile(save);
        return 0;
    }
    UseResFile(gTalkRes);
    memset(gOpenPB, 0, sizeof gOpenPB);
    pb->ioNamePtr = (StringPtr)"\p.SPEECH";
    *(const unsigned char **)((char *)gOpenPB + 60) = empty;     /* no exceptions file */
    if (PBOpenSync((ParmBlkPtr)pb) != noErr) {
        CloseResFile(gTalkRes);
        gTalkRes = -1;
        UseResFile(save);
        return 0;
    }
    gDrvRef = pb->ioRefNum;
    gReader = *(Handle *)((char *)gOpenPB + 52);
    UseResFile(save);
    if (!gReader) {
        speech_quit();
        return 0;
    }
    gOk = 1;
    control(4, 128);          /* a little higher than the default 110 Hz */
    control(2, 160);          /* and a little quicker than 150 words a minute */
    return 1;
}

short speech_available(void)
{
    return gOk;
}

void speech_say(const char *text)
{
    Handle ph;
    short save, err;
    long n;
    unsigned long t0;
    if (!gOk)
        return;
    ph = NewHandle(0);
    if (!ph)
        return;
    save = CurResFile();
    UseResFile(gTalkRes);
    HLock(gReader);
    err = call_reader(*gReader, text, (long)strlen(text), ph);
    HUnlock(gReader);
    UseResFile(save);
    n = GetHandleSize(ph);
    if (!err && n > 0) {
        IOParam pb;
        t0 = TICKS;
        while (snd_busy() && TICKS - t0 < 90)      /* let the sound effect finish */
            SystemTask();
        snd_hold(1);
        memset(&pb, 0, sizeof pb);
        pb.ioRefNum = gDrvRef;
        HLock(ph);
        pb.ioBuffer = (Ptr)ph;                     /* (the driver wants the Handle) */
        pb.ioReqCount = n;
        trap_a0(0x203, &pb);
        HUnlock(ph);
        snd_hold(0);
        FlushEvents(mDownMask | mUpMask | keyDownMask | autoKeyMask, 0);
    }
    DisposeHandle(ph);
}

void speech_quit(void)
{
    if (gDrvRef) {
        CloseDriver(gDrvRef);
        gDrvRef = 0;
    }
    if (gTalkRes != -1) {
        CloseResFile(gTalkRes);
        gTalkRes = -1;
    }
    gOk = 0;
}

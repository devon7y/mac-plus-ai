/*
 * Memory probe: how much memory does an application get? Asks for a 16 MB partition (SIZE -1
 * in memprobe.r), so under System 7 it receives the largest unused block. Writes a 'MEMP'
 * block (read by tests/check_block.lua) and shows the numbers on screen.
 */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <OSUtils.h>
#include <MacMemory.h>
#include <Processes.h>
#include <TextUtils.h>
#include <Traps.h>

#define MAGIC 0x4D454D50UL    /* 'MEMP' */

struct Block {
    unsigned long magic, version, notMagic;   /*  0 */
    unsigned long state;                      /* 12: 1 = done */
    unsigned long passBits, nChecks;          /* 16, 20 */
    long hasPM;                               /* 24: Process Manager (_OSDispatch) present */
    long processSize, processFreeMem;         /* 28, 32: from GetProcessInformation */
    long freeMem, maxMem;                     /* 36, 40: FreeMem(), MaxMem() in our heap */
    long tempFree, tempMax;                   /* 44, 48: TempFreeMem(), TempMaxMem(): outside partitions */
    long heapLimit;                           /* 52: ApplLimit - ApplZone */
    long stack;                               /* 56: StackSpace() */
    Rect bounds;                              /* 60: screenBits.bounds */
    long memTop, bufPtr, applZone, a5;        /* 68..83: low-memory globals */
};
static struct Block blk;

static short y = 50;
static void line(ConstStr255Param label, long v)
{
    Str255 num;
    NumToString(v, num);
    MoveTo(qd.screenBits.bounds.left + 30, y);
    DrawString(label);
    MoveTo(qd.screenBits.bounds.left + 260, y);
    DrawString(num);
    y += 16;
}

int main(void)
{
    Size grow;
    WindowPtr win;
    Rect r;
    EventRecord ev;

    blk.magic = MAGIC; blk.version = 1; blk.notMagic = ~MAGIC;
    MaxApplZone();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    blk.hasPM = NGetTrapAddress(0xA88F, kToolboxTrapType) != NGetTrapAddress(_Unimplemented, kToolboxTrapType);
    if (blk.hasPM) {
        ProcessSerialNumber psn;
        ProcessInfoRec info;
        GetCurrentProcess(&psn);
        info.processInfoLength = sizeof info;
        info.processName = NULL;
        info.processAppSpec = NULL;
        if (GetProcessInformation(&psn, &info) == 0) {
            blk.processSize = info.processSize;
            blk.processFreeMem = info.processFreeMem;
        }
        blk.tempFree = TempFreeMem();
        blk.tempMax = TempMaxMem(&grow);
    }
    blk.freeMem = FreeMem();
    blk.maxMem = MaxMem(&grow);
    blk.heapLimit = (long)LMGetApplLimit() - (long)LMGetApplZone();
    blk.stack = StackSpace();
    blk.bounds = qd.screenBits.bounds;
    blk.memTop = *(long *)0x108;
    blk.bufPtr = *(long *)0x10C;
    blk.applZone = *(long *)0x2AA;
    blk.a5 = (long)SetCurrentA5();

    blk.nChecks = 2;
    if (blk.maxMem > 256L * 1024) blk.passBits |= 1;            /* got a usable heap */
    if (!blk.hasPM || blk.processSize > 0) blk.passBits |= 2;   /* Process Manager info read */

    r = qd.screenBits.bounds;
    win = NewWindow(NULL, &r, "\p", true, plainDBox, (WindowPtr)-1, false, 0);
    SetPort(win);
    MoveTo(qd.screenBits.bounds.left + 30, 30);
    DrawString("\pMemory probe (bytes)");
    line("\pPartition (processSize)", blk.processSize);
    line("\pFreeMem in partition", blk.freeMem);
    line("\pMaxMem in partition", blk.maxMem);
    line("\pTempFreeMem (unused)", blk.tempFree);
    line("\pTempMaxMem (largest unused)", blk.tempMax);
    line("\pApplLimit - ApplZone", blk.heapLimit);
    line("\pStackSpace", blk.stack);
    line("\pProcess Manager present", blk.hasPM);
    blk.state = 1;

    for (;;) {
        SystemTask();
        GetNextEvent(everyEvent, &ev);
    }
    return 0;
}

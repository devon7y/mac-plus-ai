/*
 * Cycle calibration: time loops of known 68000 cycle counts with Ticks, so the emulator's
 * effective CPU clock can be computed (and compared with 7.8336 MHz). Results in a 'CYCL' block.
 *   reg loop:  subq.l #1,d0 / bne.s  = 8 + 10 = 18 cycles per iteration (code fetch from RAM)
 *   mem loop:  2x lea abs.l + 8x move.l (a0)+,(a1)+ + dbra = 24 + 160 + 10 = 194 cycles per 32 bytes
 * Interrupts (VBL, cursor) are left on, as in a game; they cost about 1%.
 */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Events.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <OSUtils.h>
#include <MacMemory.h>

#define MAGIC 0x4359434CUL   /* 'CYCL' */
#define TICKS (*(volatile unsigned long *)0x16A)

struct Block {
    unsigned long magic, version, notMagic, state;   /* 0..15 */
    unsigned long passBits, nChecks;                  /* 16, 20 */
    unsigned long regIters, regTicks;                 /* 24, 28 */
    unsigned long memIters, memTicks;                 /* 32, 36 */
};
static volatile struct Block blk;   /* volatile: only the Lua script reads it */
static unsigned long src[64], dst[64];

static void reg_loop(unsigned long n)
{
    __asm__ volatile("1: subq.l #1,%0\n\t bne.s 1b" : "+d"(n) : : "cc");
}

static void mem_loop(unsigned short n)   /* n iterations of 32 bytes */
{
    register void *a0 __asm__("a0");
    register void *a1 __asm__("a1");
    __asm__ volatile(
        "subq.w #1,%2\n"
        "1:\tlea %3,%0\n\t lea %4,%1\n\t"
        ".rept 8\n\t move.l (%0)+,(%1)+\n\t .endr\n\t"
        "dbra %2,1b"
        : "=&a"(a0), "=&a"(a1), "+d"(n) : "m"(src), "m"(dst) : "cc", "memory");
}

int main(void)
{
    unsigned long t;
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
    t = TICKS; while (TICKS == t) ;                    /* start on a tick edge */
    t = TICKS; reg_loop(2000000UL); blk.regTicks = TICKS - t; blk.regIters = 2000000UL;
    t = TICKS; while (TICKS == t) ;
    t = TICKS;
    { short k; for (k = 0; k < 20; k++) mem_loop(10000); }
    blk.memTicks = TICKS - t; blk.memIters = 200000UL;
    blk.nChecks = 1; blk.passBits = 1;
    blk.state = 1;
    for (;;) { SystemTask(); GetNextEvent(everyEvent, &ev); }
}

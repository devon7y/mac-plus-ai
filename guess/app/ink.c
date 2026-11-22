/* Guess My Doodle: the mouse, sampled by a VBL task at 60 Hz into a ring buffer, so no part of a
 * stroke is lost while the classifier runs (the main loop reads the samples when it gets to them).
 * Each sample: the cursor position (Mouse, $830: v then h) with bit 15 of h set while the button is
 * down (MBState, $172, bit 7 clear = down). Only changes are recorded. */
#include <Multiverse.h>
#include "ink.h"

#define NS 256

typedef struct {
    VBLTask t;                  /* +0 (14 bytes) */
    unsigned long *buf;         /* +14 */
    short head;                 /* +18 byte offset of the next write */
    unsigned long last;         /* +20 */
} InkTask;

static InkTask task;
static unsigned long ring[NS];
static short tail, installed;

void ink_vbl(void);
asm("   .text\n"
    "   .globl ink_vbl\n"
    "ink_vbl:\n"                        /* a0 = the task; d0-d3/a0-a3 are free in a VBL task */
    "   move.w #1,10(%a0)\n"            /* vblCount: run again next tick */
    "   move.l 0x830.w,%d0\n"           /* Mouse: v (high word), h (low word) */
    "   tst.b 0x172.w\n"                /* MBState: bit 7 set = up */
    "   bmi.s 1f\n"
    "   bset #15,%d0\n"
    "1: cmp.l 20(%a0),%d0\n"
    "   beq.s 2f\n"
    "   move.l %d0,20(%a0)\n"
    "   movea.l 14(%a0),%a1\n"
    "   move.w 18(%a0),%d1\n"
    "   move.l %d0,0(%a1,%d1.w)\n"
    "   addq.w #4,%d1\n"
    "   andi.w #1023,%d1\n"
    "   move.w %d1,18(%a0)\n"
    "2: rts\n");

int ink_init(void)
{
    task.t.qType = vType;
    task.t.vblAddr = (ProcPtr)ink_vbl;
    task.t.vblCount = 1;
    task.t.vblPhase = 0;
    task.buf = ring;
    task.head = 0;
    task.last = 0xFFFFFFFFUL;
    tail = 0;
    installed = VInstall(&task.t) == noErr;
    return installed;
}

void ink_quit(void)
{
    if (installed) VRemove(&task.t);
    installed = 0;
}

int ink_next(short *v, short *h, short *down)
{
    unsigned long s;
    if (tail == task.head) return 0;
    s = ring[tail >> 2];
    tail = (tail + 4) & 1023;
    *v = (short)(s >> 16);
    *h = (short)(s & 0x7FFF);
    if (*h & 0x4000) *h |= (short)0x8000;     /* (negative h, a cursor left of the screen: never) */
    *down = (short)((s >> 15) & 1);
    return 1;
}

int ink_pending(void)
{
    return tail != task.head;
}

void ink_flush(void)
{
    tail = task.head;
}

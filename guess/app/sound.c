/*
 * Guess My Doodle's sound effects: one voice, written straight into the Mac Plus / SE sound buffer
 * (370 words at SoundBase; the high byte of each word is a sample, played one per scan line at
 * 22,255 Hz). The sounds (build/gen/sounds.c, from guess/tools/gen_sounds.py) are 8-bit unsigned
 * (offset binary) at half that rate; each sample is written twice by a small assembly loop, about
 * 23 cycles per output sample (Broadside's two-voice C mixer, which this replaces, cost 250-400
 * cycles per sample: most of the CPU while a sound played).
 *
 * Timing as in broadside/app/sound.c and pokemon/runtime/mac/sound.c: the hardware plays positions
 * 0..369 in order and is at position 342 at the vertical blank. The VBL task writes positions
 * SPLIT..341 (the rest of the current chunk); a Time Manager task 10 ms later, when the beam is past
 * position ~190, writes positions 342..369 and 0..SPLIT-1 (the start of the next chunk). When nothing
 * plays, the buffer is left silent and the tasks only check a flag.
 * A new sound replaces the one playing unless that one has a higher priority.
 */
#include "guess.h"
#include "sound.h"

#define SPLIT 170
#define TM_DELAY 10

static VBLTask vtask;
static TMTask ttask;
static short installed, tm_installed;
static unsigned char *hw;
static volatile short tm_primed;
static const unsigned char *volatile vp;      /* the next sample */
static volatile long vleft;                   /* samples left (each one fills two positions) */
static volatile short vprio;
static volatile short active;                 /* writing the buffer */
static volatile short silent;                 /* parts written silent in a row */
static volatile short held;

static const short prio[SND_COUNT] = {0, 2, 2, 0, 1, 3};   /* tick, chime, fail, blip, start, fanfare */

/* dst[0], dst[2] = src[0]; dst[4], dst[6] = src[1]; ... (k samples); fill: dst[0], dst[2], ... = 0x80 (n) */
void snd_copy2(unsigned char *dst, const unsigned char *src, long k);
void snd_fill(unsigned char *dst, long n);
asm("   .text\n"
    "   .globl snd_copy2\n"
    "snd_copy2:\n"
    "   movea.l 4(%sp),%a0\n"
    "   movea.l 8(%sp),%a1\n"
    "   move.l 12(%sp),%d1\n"
    "   subq.w #1,%d1\n"
    "   bmi.s 2f\n"
    "1: move.b (%a1)+,%d0\n"
    "   move.b %d0,(%a0)\n"
    "   move.b %d0,2(%a0)\n"
    "   addq.l #4,%a0\n"
    "   dbra %d1,1b\n"
    "2: rts\n"
    "   .globl snd_fill\n"
    "snd_fill:\n"
    "   movea.l 4(%sp),%a0\n"
    "   move.l 8(%sp),%d1\n"
    "   subq.w #1,%d1\n"
    "   bmi.s 2f\n"
    "   moveq #-128,%d0\n"
    "1: move.b %d0,(%a0)\n"
    "   addq.l #2,%a0\n"
    "   dbra %d1,1b\n"
    "2: rts\n");

static void play_range(unsigned char *dst, short n)          /* n even */
{
    long k = n >> 1;
    if (k > vleft) k = vleft;
    snd_copy2(dst, vp, k);
    vp += k;
    vleft -= k;
    if (k < (n >> 1)) snd_fill(dst + 4 * k, n - 2 * k);
}

static void part(unsigned char *dst, short n, unsigned char *dst2, short n2)
{
    if (held || vleft <= 0) {
        snd_fill(dst, n);
        if (n2) snd_fill(dst2, n2);
        silent++;
    } else {
        play_range(dst, n);
        if (n2) play_range(dst2, n2);
        silent = 0;
    }
}

static void tm_tick(void)
{
    if (!tm_primed) return;
    tm_primed = 0;
    if (active) part(hw + 342 * 2, 28, hw, SPLIT);
}

static void vbl_tick(void)
{
    vtask.vblCount = 1;
    if (!active) {
        if (held || vleft <= 0) return;
        active = 1;
        silent = 0;
        snd_fill(hw + SPLIT * 2, 342 - SPLIT);        /* this chunk starts silent; the next is played */
    } else {
        part(hw + SPLIT * 2, 342 - SPLIT, 0, 0);
        if (silent >= 3) {                            /* the whole buffer is silent: stop writing */
            active = 0;
            return;
        }
    }
    tm_primed = 1;
    if (tm_installed) PrimeTime((QElemPtr)&ttask, TM_DELAY);
}

short snd_init(void)
{
    short i;
    hw = *(unsigned char **)0x266;                    /* SoundBase */
    if (!hw) return -1;
    for (i = 0; i < 370; i++) hw[i * 2] = 0x80;
    memset(&vtask, 0, sizeof vtask);
    vtask.qType = vType;
    vtask.vblAddr = (ProcPtr)vbl_tick;                /* (the ROM saves d0-d3/a0-a3 around the call) */
    vtask.vblCount = 1;
    memset(&ttask, 0, sizeof ttask);
    ttask.tmAddr = (ProcPtr)tm_tick;
    InsTime((QElemPtr)&ttask);
    tm_installed = 1;
    if (VInstall(&vtask) == noErr) installed = 1;
    return installed ? 0 : -1;
}

void snd_play(int which)
{
    unsigned short sr;
    if (!installed || !gSoundOn || which < 0 || which >= SND_COUNT) return;
    if (vleft > 0 && prio[which] < vprio) return;
    __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr));
    vp = snd_data + snd_table[which][0];
    vleft = (long)snd_table[which][1];
    vprio = prio[which];
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr));
}

short snd_busy(void)
{
    return vleft > 0;
}

void snd_hold(int on)
{
    unsigned long t0 = TICKS;
    if (on) {
        unsigned short sr;
        __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr));
        vleft = 0;
        held = 1;
        __asm__ volatile("move.w %0,%%sr" : : "d"(sr));
        while (active && TICKS - t0 < 10) { }          /* until the buffer is silent */
    } else {
        held = 0;
    }
}

void snd_quit(void)
{
    short i;
    if (installed) { VRemove(&vtask); installed = 0; }
    if (tm_installed) { RmvTime((QElemPtr)&ttask); tm_installed = 0; }
    if (hw) for (i = 0; i < 370; i++) hw[i * 2] = 0x80;
}

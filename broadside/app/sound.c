/*
 * Broadside's sound effects: up to two one-shot sounds at a time, mixed straight into the Mac
 * Plus / SE sound buffer (370 words at SoundBase; the high byte of each word is a sample, played
 * one per scan line at 22,255 Hz). The sounds (build/gen/sounds.c, from tools/gen_sounds.py) are
 * 8-bit signed at half that rate; each sample is played twice.
 *
 * Timing as in pokemon/runtime/mac/sound.c: the hardware plays positions 0..369 in order and is
 * at position 342 at the vertical blank. The VBL task writes positions SPLIT..341 (the rest of
 * the current chunk); a Time Manager task 10 ms later, when the beam is past position ~190, writes
 * positions 342..369 and 0..SPLIT-1 (the start of the next chunk). When nothing plays, the buffer
 * is left silent and the tasks only check a flag.
 */
#include "broadside.h"
#include "sound.h"

#define SPLIT 170
#define TM_DELAY 10

static VBLTask vtask;
static TMTask ttask;
static short installed, tm_installed;
static unsigned char *hw;
static volatile short tm_primed;

typedef struct {
    const signed char *p;
    long left;                     /* samples left (at the half rate) */
    short phase;                   /* 0/1: first or second copy of the sample */
} Voice;
static volatile Voice voice[2];
static volatile short active;      /* writing the buffer */
static volatile short silent;      /* parts written silent in a row */
static volatile short held;

/* mix n samples into dst[0], dst[2], ... */
static void mix_range(unsigned char *dst, short n)
{
    short i;
    Voice *v0 = (Voice *)&voice[0], *v1 = (Voice *)&voice[1];
    for (i = 0; i < n; i++) {
        short s = 0;
        if (v0->left > 0) {
            s += *v0->p;
            if (v0->phase) {
                v0->p++;
                v0->left--;
            }
            v0->phase ^= 1;
        }
        if (v1->left > 0) {
            s += *v1->p;
            if (v1->phase) {
                v1->p++;
                v1->left--;
            }
            v1->phase ^= 1;
        }
        if (s > 127)
            s = 127;
        else if (s < -128)
            s = -128;
        *dst = (unsigned char)(s + 128);
        dst += 2;
    }
}

static void fill_silence(unsigned char *dst, short n)
{
    while (n--) {
        *dst = 0x80;
        dst += 2;
    }
}

static short playing(void)
{
    return voice[0].left > 0 || voice[1].left > 0;
}

static void part(unsigned char *dst, short n, unsigned char *dst2, short n2)
{
    if (held || !playing()) {
        fill_silence(dst, n);
        if (n2)
            fill_silence(dst2, n2);
        silent++;
    } else {
        mix_range(dst, n);
        if (n2)
            mix_range(dst2, n2);
        silent = 0;
    }
}

static void tm_tick(void)
{
    if (!tm_primed)
        return;
    tm_primed = 0;
    if (active)
        part(hw + 342 * 2, 28, hw, SPLIT);
}

static void vbl_tick(void)
{
    vtask.vblCount = 1;
    if (!active) {
        if (held || !playing())
            return;
        active = 1;
        silent = 0;
        fill_silence(hw + SPLIT * 2, 342 - SPLIT);   /* this chunk starts silent; the next is mixed */
    } else {
        part(hw + SPLIT * 2, 342 - SPLIT, 0, 0);
        if (silent >= 3) {         /* the whole buffer is silent: stop writing */
            active = 0;
            return;
        }
    }
    tm_primed = 1;
    if (tm_installed)
        PrimeTime((QElemPtr)&ttask, TM_DELAY);
}

short snd_init(void)
{
    short i;
    hw = *(unsigned char **)0x266;              /* SoundBase */
    if (!hw)
        return -1;
    for (i = 0; i < 370; i++)
        hw[i * 2] = 0x80;
    memset(&vtask, 0, sizeof vtask);
    vtask.qType = vType;
    vtask.vblAddr = (ProcPtr)vbl_tick;           /* (the ROM saves d0-d3/a0-a3 around the call) */
    vtask.vblCount = 1;
    memset(&ttask, 0, sizeof ttask);
    ttask.tmAddr = (ProcPtr)tm_tick;
    InsTime((QElemPtr)&ttask);
    tm_installed = 1;
    if (VInstall(&vtask) == noErr)
        installed = 1;
    return installed ? 0 : -1;
}

void snd_play(int which)
{
    short v;
    unsigned short sr;
    if (!installed || !gPrefs.sound || which < 0 || which >= SND_COUNT)
        return;
    /* the free voice, else the one closer to its end */
    v = voice[0].left <= 0 ? 0 : voice[1].left <= 0 ? 1 : (voice[0].left < voice[1].left ? 0 : 1);
    __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr));
    voice[v].p = snd_data + snd_table[which][0];
    voice[v].phase = 0;
    voice[v].left = (long)snd_table[which][1];
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr));
}

short snd_busy(void)
{
    return playing();
}

void snd_hold(int on)
{
    unsigned long t0 = TICKS;
    if (on) {
        unsigned short sr;
        __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr));
        voice[0].left = voice[1].left = 0;
        held = 1;
        __asm__ volatile("move.w %0,%%sr" : : "d"(sr));
        while (active && TICKS - t0 < 10)     /* until the buffer is silent */
            ;
    } else {
        held = 0;
    }
}

void snd_quit(void)
{
    short i;
    if (installed) {
        VRemove(&vtask);
        installed = 0;
    }
    if (tm_installed) {
        RmvTime((QElemPtr)&ttask);
        tm_installed = 0;
    }
    if (hw)
        for (i = 0; i < 370; i++)
            hw[i * 2] = 0x80;
}

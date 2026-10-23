/* Blockworld sound effects: synthesized once at start (square waves, sweeps, noise) at the
 * Plus's 22257 Hz, padded to whole video frames (370 samples); a VBL task (sndvbl.s, the PvZ
 * port's design) copies one frame of the playing effect per VBL into the sound buffer, so an
 * effect costs about 3,500 cycles a frame while it plays and nothing when silent. One effect at a
 * time: a new one replaces the playing one if it ranks at least as high (higher ids rank higher).
 * M turns sound on and off. */
#include <MacMemory.h>
#include <OSUtils.h>
#include <string.h>
#include "sound.h"
#include "synth.h"

extern void snd_vbl(void);

typedef struct SndTask {
    VBLTask vbl;                  /* 14 bytes */
    short pad;
    unsigned char *src;           /* 16 */
    unsigned long frames;         /* 20 */
    unsigned char *buf;           /* 24 */
    unsigned long silent;         /* 28 */
} SndTask;

#define FRAME 370
#define RATE 22257L
#define VIA_B (*(volatile unsigned char *)0xEFE1FE)

SndTask snd_task;                 /* (tests read it: debug block +250) */
#define task snd_task
static unsigned char *data;
static unsigned long off[FX_N], frames[FX_N];
short snd_installed, playing = -1;
#define installed snd_installed
short snd_on = 1;

static void snd_enable(short on)
{
    unsigned short sr;
    __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr));
    if (on) VIA_B &= 0x7F; else VIA_B |= 0x80;
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr));
}

/* ---- synthesis -------------------------------------------------------------------------------
 * An effect is a list of parts {ms, kind, f0, f1, amp}: kind 0 silence, 1 square wave sweeping
 * f0 -> f1 Hz, 2 noise held at a rate sweeping f0 -> f1 Hz (rumbles when slow); each part fades
 * linearly to a third of its amplitude. */
typedef struct { short ms, kind, f0, f1, amp; } Part;
static const Part P_DIG[] = {{35, 2, 3000, 1500, 22}, {0}};
static const Part P_BREAK[] = {{60, 2, 2500, 900, 44}, {80, 2, 900, 400, 30}, {0}};
static const Part P_PLACE[] = {{50, 1, 160, 110, 40}, {0}};
static const Part P_CRAFT[] = {{40, 1, 900, 900, 26}, {10, 0, 0, 0, 0}, {50, 1, 1300, 1300, 26}, {0}};
static const Part P_EAT[] = {{50, 2, 1800, 1200, 30}, {50, 0, 0, 0, 0}, {50, 2, 1800, 1200, 30},
                             {50, 0, 0, 0, 0}, {60, 2, 1600, 1000, 30}, {0}};
static const Part P_BOW[] = {{90, 1, 500, 1000, 30}, {0}};
static const Part P_HISS[] = {{700, 2, 7000, 6000, 18}, {0}};
static const Part P_HIT[] = {{70, 1, 320, 260, 46}, {0}};
static const Part P_DIE[] = {{110, 1, 330, 220, 46}, {150, 1, 220, 90, 40}, {0}};
static const Part P_HURT[] = {{90, 1, 420, 300, 54}, {120, 1, 300, 170, 46}, {0}};
static const Part P_BOOM[] = {{120, 2, 3000, 800, 70}, {300, 2, 800, 150, 60}, {350, 2, 150, 60, 45}, {0}};
static const Part *const fx_parts[FX_N] = {P_DIG, P_PLACE, P_CRAFT, P_HISS, P_EAT, P_BOW, P_BREAK,
                                           P_HIT, P_DIE, P_HURT, P_BOOM};

/* an effect's samples into out; frequency and amplitude step linearly (16.16 fixed point: no
   divisions per sample) */
static unsigned long synth_len(const Part *p)
{
    unsigned long n = 0;
    for (; p->ms; p++) n += RATE * p->ms / 1000;
    return n;
}

static void synth(const Part *p, unsigned char *out)
{
    unsigned short lfsr = 0xACE1;
    for (; p->ms; p++) {
        long len = RATE * p->ms / 1000;
        long df = ((long)(p->f1 - p->f0) << 16) / len, da = -(((long)p->amp * 2) << 16) / (3 * len);
        synth_part(out, len, (long)p->f0 << 16, df, (long)p->amp << 16, da, p->kind, &lfsr);
        out += len;
    }
}

void snd_init(void)
{
    unsigned long total = 0, i, n;
    for (i = 0; i < FX_N; i++) {
        n = synth_len(fx_parts[i]);
        frames[i] = (n + FRAME - 1) / FRAME;
        off[i] = total;
        total += frames[i] * FRAME;
    }
    data = (unsigned char *)NewPtr((long)total);
    if (!data) return;
    for (i = 0; i < FX_N; i++) {
        n = synth_len(fx_parts[i]);
        synth(fx_parts[i], data + off[i]);
        for (; n < frames[i] * FRAME; n++) data[off[i] + n] = 128;
    }
    memset(&task, 0, sizeof task);
    task.buf = *(unsigned char **)0x266;      /* SoundBase */
    task.vbl.qType = 1;                       /* vType */
    task.vbl.vblAddr = (ProcPtr)snd_vbl;
    task.vbl.vblCount = 1;
    snd_enable(1);
    if (VInstall(&task.vbl) == noErr) installed = 1;
}

void snd_play(short id)
{
    if (!installed || !snd_on || id < 0 || id >= FX_N) return;
    if (task.frames && playing > id) return;          /* a higher-ranking effect is playing */
    task.frames = 0;                                   /* (the VBL task ignores a zero count) */
    task.src = data + off[id];
    task.frames = frames[id];
    playing = id;
}

void snd_toggle(void)
{
    snd_on = (short)!snd_on;
    if (!snd_on) task.frames = 0;
}

void snd_quit(void)
{
    if (installed) { VRemove(&task.vbl); installed = 0; }
    if (task.buf) { short i; for (i = 0; i < FRAME; i++) task.buf[i * 2] = 0x80; }
}

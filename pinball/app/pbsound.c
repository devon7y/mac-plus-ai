/* Neural Pinball sound: short square-wave and noise blips written straight into the Plus's
 * sound buffer (370 samples, one per scan line, played by DMA once per video frame).
 *
 * Same approach as Rail Runner (runner/game/sound.c): an effect is a list of steps {half
 * period in samples (0 = rest, -1 = noise), amplitude, ticks}; a step change copies a cached
 * 370-sample waveform into the buffer (about 5000 cycles), and nothing runs in between. Steps
 * advance from sound_tick(), once per game tick. */
#include <MacMemory.h>
#include "pbsound.h"

#define SND_N 370
#define VIA_B (*(volatile unsigned char *)via_b_addr)
#define TICKS (*(volatile unsigned long *)ticks_addr)
#define SOUND_BASE (*(unsigned char *volatile *)soundbase_addr)
static volatile long via_b_addr = 0xEFE1FE, ticks_addr = 0x16A, soundbase_addr = 0x266;

static unsigned char *sndbuf;
static const short *cur;
static unsigned long step_end;
static short cur_prio, muted, silent = 1;

/* {half period (samples; -1 noise), amplitude, ticks}, ..., ticks 0 ends */
static const short fx_bumper[]  = {-1, 50, 2, 14, 46, 2, 18, 40, 2, 0, 0, 0};
static const short fx_sling[]   = {-1, 40, 1, 9, 36, 2, 0, 0, 0};
static const short fx_flipper[] = {-1, 22, 1, 40, 24, 1, 0, 0, 0};
static const short fx_drain[]   = {30, 40, 6, 38, 40, 6, 48, 40, 6, 62, 40, 10, 0, 0, 0};
static const short fx_lane[]    = {10, 36, 3, 8, 36, 3, 0, 0, 0};
static const short fx_ramp[]    = {20, 36, 3, 16, 36, 3, 13, 36, 3, 10, 36, 3, 8, 36, 6, 0, 0, 0};
static const short fx_drop[]    = {-1, 44, 1, 24, 40, 2, 0, 0, 0};
static const short fx_jackpot[] = {16, 44, 5, 12, 44, 5, 10, 44, 5, 8, 44, 5, 0, 0, 2, 8, 44, 5,
                                   6, 44, 12, 0, 0, 0};
static const short fx_multi[]   = {12, 40, 4, 9, 40, 4, 12, 40, 4, 9, 40, 4, 12, 40, 4, 9, 40, 4,
                                   7, 44, 14, 0, 0, 0};
static const short fx_tilt[]    = {70, 56, 30, 0, 0, 0};
static const short fx_launch[]  = {-1, 46, 3, -1, 30, 3, 0, 0, 0};
static const short fx_spin[]    = {7, 26, 1, 0, 0, 0};
static const short fx_sink[]    = {24, 44, 2, 34, 44, 3, 46, 40, 4, 0, 0, 0};
static const short fx_kick[]    = {-1, 54, 2, 30, 44, 2, 0, 0, 0};
static const short fx_wall[]    = {-1, 18, 1, 0, 0, 0};
static const short fx_fanfare[] = {19, 40, 6, 15, 40, 6, 12, 40, 6, 10, 44, 16, 0, 0, 0};
static const short fx_start[]   = {19, 40, 8, 0, 0, 2, 19, 40, 8, 15, 40, 8, 12, 40, 8, 0, 0, 4,
                                   15, 40, 8, 12, 40, 16, 0, 0, 0};

static void snd_enable(short on)
{
    unsigned short sr;
    __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr));
    if (on) VIA_B &= 0x7F; else VIA_B |= 0x80;
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr));
}

#define MAX_WAVES 40
static short wave_key[MAX_WAVES], nwaves;
static unsigned char *wave_buf[MAX_WAVES];
static unsigned char wave_silence[SND_N + 2] __attribute__((aligned(4)));

static void gen(unsigned char *w, short half, short amp)
{
    short i, ph = 0;
    unsigned short lfsr = 0xACE1;
    unsigned char hi = (unsigned char)(128 + amp), lo = (unsigned char)(128 - amp), v = hi;
    for (i = 0; i < SND_N + 2; i++) {
        if (amp <= 0 || half == 0) {
            w[i] = 128;
            continue;
        }
        if (half < 0) {                       /* noise: a 16-bit LFSR, new value every 2 samples */
            if (!(i & 1)) {
                unsigned short bit = (unsigned short)(((lfsr >> 0) ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1);
                lfsr = (unsigned short)((lfsr >> 1) | (bit << 15));
            }
            w[i] = (lfsr & 1) ? hi : lo;
            continue;
        }
        w[i] = v;
        if (++ph >= half) {
            ph = 0;
            v = v == hi ? lo : hi;
        }
    }
}

static const unsigned char *wave(short half, short amp)
{
    short k = (short)(((half & 0xFF) << 8) | amp), i;
    if (half == 0 || amp <= 0)
        return wave_silence;
    for (i = 0; i < nwaves; i++)
        if (wave_key[i] == k)
            return wave_buf[i];
    if (nwaves >= MAX_WAVES)
        return wave_silence;
    wave_buf[nwaves] = (unsigned char *)NewPtr(SND_N + 2);
    if (!wave_buf[nwaves])
        return wave_silence;
    gen(wave_buf[nwaves], half, amp);
    wave_key[nwaves] = k;
    return wave_buf[nwaves++];
}

static void put(const unsigned char *w)
{
    unsigned char *p = sndbuf;
    __asm__ volatile(
        "moveq #91,%%d1\n"
        "1:\n\t"
        "move.l (%1)+,%%d0\n\t"
        "movep.l %%d0,0(%0)\n\t"
        "addq.l #8,%0\n\t"
        "dbra %%d1,1b\n\t"
        "move.b (%1)+,(%0)\n\t"
        "move.b (%1)+,2(%0)\n\t"
        : "+a"(p), "+a"(w)
        :
        : "d0", "d1", "cc", "memory");
}

static void snd_fill(short half, short amp)
{
    short off = half == 0 || amp <= 0;
    if (!sndbuf || (off && silent))
        return;
    put(wave(half, amp));
    silent = off;
}

static void play(const short *fx, short prio)
{
    if (muted || !sndbuf)
        return;
    if (cur && prio < cur_prio)
        return;
    snd_fill(fx[0], fx[1]);
    step_end = TICKS + fx[2];
    cur = fx + 3;
    cur_prio = prio;
}

void sound_init(void)
{
    static const short *const all[] = {fx_bumper, fx_sling, fx_flipper, fx_drain, fx_lane, fx_ramp, fx_drop,
                                       fx_jackpot, fx_multi, fx_tilt, fx_launch, fx_spin, fx_sink, fx_kick,
                                       fx_wall, fx_fanfare, fx_start};
    short i;
    const short *f;
    gen(wave_silence, 0, 0);
    for (i = 0; i < (short)(sizeof all / sizeof all[0]); i++)
        for (f = all[i]; f[2] != 0; f += 3)
            wave(f[0], f[1]);
    silent = 0;
    sndbuf = SOUND_BASE;
    snd_fill(0, 0);
    snd_enable(1);
    cur = 0;
    cur_prio = 0;
}

void sound_events(unsigned short ev)
{
    if (!ev)
        return;
    if (ev & SND_JACKPOT) play(fx_jackpot, 9);
    else if (ev & SND_MULTI) play(fx_multi, 9);
    else if (ev & SND_FANFARE) play(fx_fanfare, 8);
    else if (ev & SND_TILT) play(fx_tilt, 8);
    else if (ev & SND_DRAIN) play(fx_drain, 7);
    else if (ev & SND_RAMP) play(fx_ramp, 6);
    else if (ev & SND_SINK) play(fx_sink, 6);
    else if (ev & SND_KICK) play(fx_kick, 5);
    else if (ev & SND_LAUNCH) play(fx_launch, 5);
    else if (ev & SND_BUMPER) play(fx_bumper, 4);
    else if (ev & SND_DROP) play(fx_drop, 4);
    else if (ev & SND_LANE) play(fx_lane, 4);
    else if (ev & SND_SLING) play(fx_sling, 3);
    else if (ev & SND_SPIN) play(fx_spin, 2);
    else if (ev & SND_FLIPPER) play(fx_flipper, 2);
    else if (ev & SND_WALL) play(fx_wall, 1);
}

void sound_start_tune(void)
{
    play(fx_start, 10);
}

void sound_tick(void)
{
    if (!cur || (long)(TICKS - step_end) < 0)
        return;
    if (cur[2] == 0) {
        snd_fill(0, 0);
        cur = 0;
        cur_prio = 0;
        return;
    }
    snd_fill(cur[0], cur[1]);
    step_end += cur[2];
    cur += 3;
}

/* after MacinTalk, which writes the sound buffer itself: make it silent and ours again */
void sound_reset(void)
{
    silent = 0;
    cur = 0;
    cur_prio = 0;
    if (sndbuf) {
        snd_fill(0, 0);
        snd_enable(1);
    }
}

void sound_stop(void)
{
    snd_fill(0, 0);
    cur = 0;
    cur_prio = 0;
}

void sound_off(void)
{
    sound_stop();
    snd_enable(0);
}

void sound_set(short on)
{
    muted = !on;
    if (muted)
        sound_stop();
}

short sound_is_on(void)
{
    return !muted;
}

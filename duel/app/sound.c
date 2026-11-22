/* Orbit Duel sound: square-wave blips and noise written straight into the sound buffer.
 *
 * Same method as Rail Runner (runner/game/sound.c, this repo): the Plus and SE play their 370-word
 * sound buffer (sample = high byte of each word, 22.25 kHz) by DMA, over and over, so an effect is
 * a list of steps {half period in samples, amplitude, ticks}; the buffer is rewritten only when a
 * step changes (370 bytes, well under 1 ms) and costs nothing in between. A negative half period
 * is noise held for that many samples (explosions, the engine). */
#include <MacMemory.h>
#include "sound.h"

#define SND_N 370
#define VIA_B (*(volatile unsigned char *)via_b_addr)
#define TICKS (*(volatile unsigned long *)ticks_addr)
#define SOUND_BASE (*(unsigned char *volatile *)soundbase_addr)
static volatile long via_b_addr = 0xEFE1FE, ticks_addr = 0x16A, soundbase_addr = 0x266;

static unsigned char *sndbuf;
static const short *cur;
static unsigned long step_end;
static short cur_prio, muted, silent = 1, engine;

/* {half period (<0: noise), amplitude, ticks} ..., ticks 0 ends */
static const short fx_fire[]  = {7, 34, 1, 9, 34, 1, 12, 30, 1, 16, 24, 1, 0, 0, 0};
static const short fx_boom[]  = {-2, 70, 3, -3, 60, 4, -4, 48, 5, -5, 38, 6, -7, 28, 8, -9, 18, 10, -12, 10, 10, 0, 0, 0};
static const short fx_hyper[] = {44, 30, 2, 33, 30, 2, 25, 30, 2, 19, 30, 2, 14, 30, 2, 10, 30, 2, 7, 26, 3, 0, 0, 0};
static const short fx_back[]  = {7, 26, 2, 10, 30, 2, 14, 30, 2, 19, 30, 2, 0, 0, 0};
static const short fx_round[] = {19, 36, 6, 0, 0, 1, 15, 36, 6, 0, 0, 1, 12, 36, 10, 0, 0, 0};
static const short fx_match[] = {19, 40, 8, 0, 0, 2, 15, 40, 8, 12, 40, 8, 0, 0, 2, 10, 40, 8, 12, 40, 6,
                                 10, 40, 18, 0, 0, 0};
static const short fx_link[]  = {12, 36, 4, 0, 0, 2, 9, 36, 8, 0, 0, 0};
static const short fx_click[] = {10, 30, 2, 0, 0, 0};
static const short fx_engine[] = {-6, 14, 1, 0, 0, 0};

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
static unsigned char wave_silence[SND_N + 2];

static void gen(unsigned char *w, short half, short amp)
{
    short i, ph = 0;
    unsigned short lfsr = 0xACE1;
    unsigned char hi = (unsigned char)(128 + amp), lo = (unsigned char)(128 - amp), v = hi;
    for (i = 0; i < SND_N + 2; i++) {
        if (half == 0 || amp <= 0) {
            w[i] = 128;
            continue;
        }
        w[i] = v;
        if (++ph >= (half < 0 ? -half : half)) {
            ph = 0;
            if (half > 0)
                v = v == hi ? lo : hi;
            else {
                lfsr = (lfsr >> 1) ^ (-(lfsr & 1) & 0xB400u);
                v = (unsigned char)(128 + (short)((lfsr & 0xFF) - 128) * amp / 128);
            }
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

static void put(const unsigned char *w)       /* into the even bytes of the sound buffer */
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

static const unsigned char *now_wave;

static void snd_fill(short half, short amp)
{
    short off = half == 0 || amp <= 0;
    const unsigned char *w;
    if (!sndbuf || (off && silent))
        return;
    w = wave(half, amp);
    if (w != now_wave || off)
        put(w);
    now_wave = w;
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
    static const short *const all[] = {fx_fire, fx_boom, fx_hyper, fx_back, fx_round, fx_match, fx_link,
                                       fx_click, fx_engine};
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

void sound_tick(void)
{
    if (!sndbuf)
        return;
    if (cur && (long)(TICKS - step_end) >= 0) {
        if (cur[2] == 0) {
            cur = 0;
            cur_prio = 0;
            snd_fill(0, 0);
        } else {
            snd_fill(cur[0], cur[1]);
            step_end += cur[2];
            cur += 3;
        }
    }
    if (!cur && !muted)
        snd_fill(engine ? fx_engine[0] : 0, engine ? fx_engine[1] : 0);
}

void sound_engine(short on)
{
    engine = on;
}

void sound_event(short ev)
{
    switch (ev) {
    case SND_FIRE: play(fx_fire, 1); break;
    case SND_BOOM: play(fx_boom, 5); break;
    case SND_HYPER: play(fx_hyper, 3); break;
    case SND_BACK: play(fx_back, 3); break;
    case SND_ROUND: play(fx_round, 6); break;
    case SND_MATCH: play(fx_match, 7); break;
    case SND_LINK: play(fx_link, 7); break;
    case SND_CLICK: play(fx_click, 2); break;
    }
}

void sound_stop(void)
{
    snd_fill(0, 0);
    cur = 0;
    cur_prio = 0;
    engine = 0;
}

void sound_off(void)
{
    sound_stop();
    snd_enable(0);
}

void sound_set_muted(short m)
{
    muted = m;
    if (muted)
        sound_stop();
}

short sound_muted(void)
{
    return muted;
}

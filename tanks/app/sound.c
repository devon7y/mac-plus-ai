/* Tank Duel sound: effects written straight into the Plus's 370-sample sound buffer.
 *
 * Same approach as Rail Runner (runner/game/sound.c): the hardware replays the buffer every video
 * frame, so a step of an effect costs one buffer write (about 5,000 cycles with movep) and nothing
 * in between. A step is {half period, amplitude, ticks}: a positive half period is a square wave of
 * that many samples per half cycle; a negative one is noise held for -half samples (lower = hissier).
 * When no effect plays, the engine hums while the player's tank moves (two alternating buffers). */
#include <MacMemory.h>
#include "sound.h"

#define SND_N 370
#define VIA_B (*(volatile unsigned char *)via_b_addr)
#define TICKS (*(volatile unsigned long *)ticks_addr)
#define SOUND_BASE (*(unsigned char *volatile *)soundbase_addr)
static volatile long via_b_addr = 0xEFE1FE, ticks_addr = 0x16A, soundbase_addr = 0x266;

static unsigned char *sndbuf;
static const short *cur;
static unsigned long step_end, eng_next;
static short cur_prio, muted, silent = 1, engine, eng_phase;

static const short fx_fire[]    = {-2, 52, 2, -3, 44, 2, -5, 32, 2, -8, 20, 2, 0, 0, 0};
static const short fx_efire[]   = {-3, 40, 2, -5, 28, 2, -8, 16, 2, 0, 0, 0};
static const short fx_ricochet[] = {7, 30, 2, 9, 28, 2, 11, 24, 2, 14, 16, 2, 0, 0, 0};
static const short fx_boom[]    = {-5, 72, 4, -8, 64, 5, -11, 54, 6, -15, 40, 8, -20, 26, 10, -26, 14, 8, 0, 0, 0};
static const short fx_clang[]   = {6, 40, 2, 9, 40, 2, 6, 30, 3, 0, 0, 0};
static const short fx_shells[]  = {-2, 44, 2, 8, 30, 3, 0, 0, 0};
static const short fx_dud[]     = {-12, 26, 3, 0, 0, 0};
static const short fx_puff[]    = {-4, 24, 2, -7, 14, 2, 0, 0, 0};
static const short fx_start[]   = {30, 36, 6, 0, 0, 2, 30, 36, 6, 24, 36, 6, 20, 36, 12, 0, 0, 0};
static const short fx_clear[]   = {24, 36, 5, 20, 36, 5, 16, 36, 5, 12, 40, 14, 0, 0, 0};
static const short fx_over[]    = {24, 36, 10, 30, 36, 10, 36, 36, 10, 48, 36, 24, 0, 0, 0};
static const short fx_respawn[] = {20, 30, 3, 16, 30, 3, 12, 30, 5, 0, 0, 0};
static const short eng_a[]      = {104, 12};
static const short eng_b[]      = {116, 7};

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
        if (half == 0 || amp <= 0) {
            w[i] = 128;
            continue;
        }
        if (half < 0) {                       /* noise, held for -half samples */
            if (ph == 0) {
                lfsr = (unsigned short)((lfsr >> 1) ^ (-(short)(lfsr & 1) & 0xB400));
                v = (lfsr & 1) ? hi : lo;
            }
            w[i] = v;
            if (++ph >= -half)
                ph = 0;
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
    short k = (short)((half << 8) | (amp & 0xFF)), i;
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
    static const short *const all[] = {fx_fire, fx_efire, fx_ricochet, fx_boom, fx_clang, fx_shells, fx_dud,
                                       fx_puff, fx_start, fx_clear, fx_over, fx_respawn, eng_a, eng_b};
    short i;
    const short *f;
    gen(wave_silence, 0, 0);
    for (i = 0; i < (short)(sizeof all / sizeof all[0]); i++) {
        if (all[i] == eng_a || all[i] == eng_b) {
            wave(all[i][0], all[i][1]);
            continue;
        }
        for (f = all[i]; f[2] != 0; f += 3)
            wave(f[0], f[1]);
    }
    silent = 0;
    sndbuf = SOUND_BASE;
    snd_fill(0, 0);
    snd_enable(1);
    cur = 0;
    cur_prio = 0;
}

void sound_play(short what)
{
    switch (what) {
    case SND_FIRE: play(fx_fire, 3); break;
    case SND_EFIRE: play(fx_efire, 2); break;
    case SND_RICOCHET: play(fx_ricochet, 2); break;
    case SND_BOOM: play(fx_boom, 6); break;
    case SND_CLANG: play(fx_clang, 3); break;
    case SND_SHELLS: play(fx_shells, 3); break;
    case SND_DUD: play(fx_dud, 1); break;
    case SND_PUFF: play(fx_puff, 1); break;
    case SND_START: play(fx_start, 9); break;
    case SND_CLEAR: play(fx_clear, 9); break;
    case SND_OVER: play(fx_over, 9); break;
    case SND_RESPAWN: play(fx_respawn, 7); break;
    }
}

void sound_engine(short on)
{
    engine = on && !muted;
}

void sound_tick(void)
{
    unsigned long now = TICKS;
    if (cur && (long)(now - step_end) >= 0) {
        if (cur[2] == 0) {
            cur = 0;
            cur_prio = 0;
            snd_fill(0, 0);
            eng_next = now;
        } else {
            snd_fill(cur[0], cur[1]);
            step_end += cur[2];
            cur += 3;
        }
    }
    if (cur)
        return;
    if (!engine) {
        snd_fill(0, 0);
        return;
    }
    if ((long)(now - eng_next) >= 0) {        /* chug: alternate two buffers every 4 ticks */
        const short *e = (eng_phase ^= 1) ? eng_a : eng_b;
        snd_fill(e[0], e[1]);
        silent = 0;
        eng_next = now + 4;
    }
}

void sound_stop(void)
{
    cur = 0;
    cur_prio = 0;
    engine = 0;
    snd_fill(0, 0);
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

short sound_on(void)
{
    return !muted;
}

/* Rail Runner sound: short square-wave blips written straight into the Plus's sound buffer.
 *
 * The Plus plays its 370-word sound buffer (sample = high byte of each word, 22.25 kHz, one
 * buffer per video frame) by DMA, over and over. An effect is a list of steps {half period in
 * samples (0 = rest), amplitude, duration in ticks}; a step change rewrites the buffer once
 * (370 byte writes, well under 1 ms), and between changes the sound costs no CPU at all. Steps
 * advance from sound_update(), called once per rendered frame and from the idle loops, so their
 * lengths are rounded to frames. Same approach as Endurance (racer/game/main.c). The waveform
 * restarts every frame, which adds a faint 60 Hz buzz under the blips. */
#include <MacMemory.h>
#include "sound.h"

#define SND_N 370
#define VIA_B (*(volatile unsigned char *)via_b_addr)
#define TICKS (*(volatile unsigned long *)ticks_addr)
#define SOUND_BASE (*(unsigned char *volatile *)soundbase_addr)
static volatile long via_b_addr = 0xEFE1FE, ticks_addr = 0x16A, soundbase_addr = 0x266;

static unsigned char *sndbuf;
static const short *cur;            /* next step of the playing effect (0 = none) */
static unsigned long step_end;      /* Ticks when the current step ends */
static short cur_prio, muted, silent = 1;

/* effects: {half period, amplitude, ticks} ..., terminated by ticks 0 */
static const short fx_coin[]    = {6, 40, 5, 0, 0, 0};
static const short fx_jump[]    = {22, 36, 2, 18, 36, 2, 14, 36, 2, 11, 36, 2, 0, 0, 0};
static const short fx_roll[]    = {12, 36, 2, 16, 36, 2, 21, 36, 2, 28, 36, 2, 0, 0, 0};
static const short fx_stumble[] = {40, 50, 4, 0, 0, 2, 52, 50, 6, 0, 0, 0};
static const short fx_crash[]   = {45, 60, 4, 60, 60, 5, 80, 60, 6, 105, 60, 8, 140, 50, 12, 0, 0, 0};
static const short fx_power[]   = {16, 40, 4, 12, 40, 4, 10, 40, 4, 8, 40, 8, 0, 0, 0};
static const short fx_title[]   = {19, 40, 8, 0, 0, 2, 19, 40, 8, 15, 40, 8, 12, 40, 8,
                                   0, 0, 4, 15, 40, 8, 12, 40, 16, 0, 0, 0};

static void snd_enable(short on)
{
    unsigned short sr;
    __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr));
    if (on) VIA_B &= 0x7F; else VIA_B |= 0x80;
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr));
}

/* Waveforms are generated once (sound_init) into a cache of 372-byte buffers, one per distinct
 * {half period, amplitude}, and copied into the sound buffer's even bytes with movep.l
 * (4 samples per instruction, about 5000 cycles per change). */
#define MAX_WAVES 32
static short wave_key[MAX_WAVES], nwaves;
static unsigned char *wave_buf[MAX_WAVES];
static unsigned char wave_silence[SND_N + 2] __attribute__((aligned(4)));

static void gen(unsigned char *w, short half, short amp)
{
    short i, ph = 0;
    unsigned char hi = (unsigned char)(128 + amp), lo = (unsigned char)(128 - amp), v = hi;
    for (i = 0; i < SND_N + 2; i++) {
        if (half <= 0 || amp <= 0) {
            w[i] = 128;
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
    short k = (half << 8) | amp, i;
    if (half <= 0 || amp <= 0)
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

/* copy a 370-sample waveform into the sound buffer (sample = even byte of each word) */
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

/* square wave of the given half period (samples) and amplitude around 128; 0 = silence */
static void snd_fill(short half, short amp)
{
    short off = half <= 0 || amp <= 0;
    if (!sndbuf || (off && silent))
        return;
    put(wave(half, amp));
    silent = off;
}

static void play(const short *fx, short prio)
{
    if (muted || !sndbuf)
        return;
    if (cur && (prio < cur_prio || (prio == 1 && cur_prio == 1)))
        return;                          /* (a coin blip is not restarted by the next coin) */
    snd_fill(fx[0], fx[1]);
    step_end = TICKS + fx[2];
    cur = fx + 3;
    cur_prio = prio;
}

void sound_init(void)
{
    static const short *const all[] = {fx_coin, fx_jump, fx_roll, fx_stumble, fx_crash, fx_power, fx_title};
    short i;
    const short *f;
    gen(wave_silence, 0, 0);
    for (i = 0; i < (short)(sizeof all / sizeof all[0]); i++)
        for (f = all[i]; f[2] != 0; f += 3)
            wave(f[0], f[1]);                /* build every waveform now, not mid-game */
    silent = 0;
    sndbuf = SOUND_BASE;
    snd_fill(0, 0);
    snd_enable(1);
    cur = 0;
    cur_prio = 0;
#ifdef SILENT
    muted = 1;
#endif
}

void sound_events(short ev)
{
    if (!ev)
        return;
    if (ev & 8)       play(fx_crash, 5);
    else if (ev & 32) play(fx_power, 4);
    else if (ev & 16) play(fx_stumble, 3);
    else if (ev & 2)  play(fx_jump, 2);
    else if (ev & 4)  play(fx_roll, 2);
    else if (ev & 1)  play(fx_coin, 1);
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

void sound_update(const Game *g)
{
    (void)g;
    sound_tick();
}

void sound_title(void)
{
    play(fx_title, 9);
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

void sound_toggle(void)
{
    muted = !muted;
    if (muted)
        sound_stop();
}

short sound_muted(void)
{
    return muted;
}

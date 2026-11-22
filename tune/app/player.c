/*
 * Tune's player. The main program appends notes in performance order (repeats already expanded) to
 * two lists, melody and bass, and marks how far they are complete; a VBL task runs the sequencer
 * once per video frame (tempo kept by the clock, whatever the composer is doing) and, with a Time
 * Manager task, mixes the two voices straight into the Plus's sound buffer.
 *
 * Sound buffer timing (as in pokemon/runtime/mac/sound.c): the hardware reads one sample per scan
 * line, 370 per frame, and is at position 342 at the vertical blank. The Time Manager task, ~10 ms
 * after the VBL task, mixes the first 198 samples of the next chunk into positions 342..369 and
 * 0..169 (already read in this frame); the VBL task mixes the other 172 into 170..341 before the
 * beam reaches them. Samples are computed at half rate (11.1 kHz) and written twice (mix68k.s).
 */
#include <Multiverse.h>
#include <Timer.h>
#include <OSUtils.h>
#include <MacMemory.h>
#include "player.h"

typedef struct MixState {
    const unsigned char *mtab;      /* 0 */
    unsigned short mfrac, midx;     /* 4, 6 */
    unsigned short mstepf, mstepi;  /* 8, 10 */
    const signed char *btab;        /* 12: middle of the 64 KB bass table */
    unsigned short bphase, bstep;   /* 16, 18 */
} MixState;

extern void snd_vbl_glue(void);
extern void snd_tm_glue(void);
extern void mix_pairs(unsigned char *dst, long npairs, MixState *s);
extern const unsigned long pl_mstep[128];
extern const unsigned short pl_bstep[128];
extern const signed char pl_wave[2][256], pl_bwave[256], pl_vib[16];

#define TM_DELAY 10
#define MEL_AMP 76
#define BAS_AMP 40
#define TICKS (*(volatile unsigned long *)0x16A)
#define VIA_B (*(volatile unsigned char *)0xEFE1FE)

static VBLTask vtask;
static TMTask ttask;
static short installed, tm_installed;
static unsigned char *hw;
static unsigned char *levels;         /* 2 timbres x 16 levels x 256: $80 + scaled wave */
static signed char *btable;           /* 64 KB */
static MixState mx;
static volatile short tm_primed;

static PNote mel[PL_MAXP], bas[PL_MAXP];
static volatile short nmel, nbas;
static volatile long ready;           /* performance ticks complete */
static volatile short finished;
static volatile short playing;        /* sequencer running */
static volatile short sound_on = 1, bass_on = 1, timbre;
static volatile short mixing, silent_parts;
static volatile short cur_w = -1;

/* sequencer state (interrupt time) */
static unsigned long t16;             /* performance time, ticks in 16.16 */
static unsigned long tpf = 1L << 16;  /* ticks per frame, 16.16 */
static short mi, bi;
static long mend, bend, mgate;
static short level, nframes;
static unsigned long base_step;
static short vib;

static void set_melody_step(unsigned long st)
{
    mx.mstepf = (unsigned short)st;
    mx.mstepi = (unsigned short)(st >> 16);
}

static void set_level(short l)
{
    level = l;
    mx.mtab = levels + ((long)timbre * 16 + l) * 256;
}

static void silence_all(void)
{
    set_level(0);
    mx.bstep = 0;
    mx.bphase = 0;
}

/* one video frame of the sequencer: called by the VBL task between two chunks */
static void seq_frame(void)
{
    long now;
    if (!playing) {
        if (level > 0)
            set_level(level > 6 ? level - 6 : 0);
        mx.bstep = 0;
        mx.bphase = 0;
        cur_w = -1;
        return;
    }
    /* time advances only through complete material (else wait: the composer is behind) */
    if ((long)((t16 + tpf) >> 16) <= ready || finished)
        t16 += tpf;
    now = (long)(t16 >> 16);
    /* melody */
    while (now >= mend) {
        if (mi + 1 >= nmel || mend + mel[mi + 1].dur > ready) {
            if (finished && mi + 1 >= nmel) {
                playing = 0;          /* the end: let the last note ring out */
                return;
            }
            break;                    /* wait */
        }
        mi++;
        mend += mel[mi].dur;
        if (mel[mi].midi) {
            short d = mel[mi].dur;
            mgate = mend - (d >= 8 ? 2 : d >= 4 ? 1 : 0);
            base_step = pl_mstep[mel[mi].midi];
            set_melody_step(base_step);
            set_level(12);
            nframes = 0;
            vib = 0;
        } else {
            mgate = mend - mel[mi].dur;
        }
        cur_w = mel[mi].wnote;
    }
    if (now >= mgate || mi < 0) {
        if (level > 0)
            set_level(level > 5 ? level - 5 : 0);
    } else {
        nframes++;
        if (nframes == 1)
            set_level(15);
        else if ((nframes & 7) == 0 && level > 11)
            set_level(level - 1);
        if (nframes > 12) {           /* vibrato after 0.2 s: about +-0.35 %, 5.6 Hz */
            short bs = (short)(base_step >> 9);
            long d = ((long)bs * pl_vib[vib & 15]) >> 7;
            vib += 1 + (nframes & 1);
            set_melody_step(base_step + d);
        }
    }
    /* bass */
    while (now >= bend) {
        if (bi + 1 >= nbas || bend + bas[bi + 1].dur > ready)
            break;
        bi++;
        bend += bas[bi].dur;
        if (bas[bi].midi && bass_on) {
            if (mx.bstep == 0)
                mx.bphase = 0;
            mx.bstep = pl_bstep[bas[bi].midi];
        } else {
            mx.bstep = 0;
            mx.bphase = 0;
        }
    }
}

static void fill_hw(unsigned char *dst, short n)
{
    while (n-- > 0) {
        *dst = 0x80;
        dst += 2;
    }
}

void snd_tm_tick(void)
{
    if (!tm_primed)
        return;
    tm_primed = 0;
    if (mixing) {
        mix_pairs(hw + 342 * 2, 14, &mx);
        mix_pairs(hw, 85, &mx);
        silent_parts = 0;
    } else if (silent_parts < 2) {
        fill_hw(hw + 342 * 2, 28);
        fill_hw(hw, 170);
        silent_parts++;
    }
}

void snd_vbl_tick(void)
{
    vtask.vblCount = 1;
    if (mixing) {
        mix_pairs(hw + 170 * 2, 86, &mx);
        silent_parts = 0;
    } else if (silent_parts < 2) {
        fill_hw(hw + 170 * 2, 172);
        silent_parts++;
    }
    seq_frame();
    mixing = sound_on && (playing || level > 0);
    tm_primed = 1;
    if (tm_installed)
        PrimeTime((QElemPtr)&ttask, TM_DELAY);
}

/* ---- main program ---------------------------------------------------------------------------- */

short pl_init(void)
{
    short t, v, i;
    long k;
    unsigned short sr;
    hw = *(unsigned char **)0x266;           /* SoundBase */
    if (!hw)
        return -1;
    levels = (unsigned char *)NewPtr(2L * 16 * 256);
    btable = (signed char *)NewPtr(65536L);
    if (!levels || !btable)
        return -2;
    for (t = 0; t < 2; t++)
        for (v = 0; v < 16; v++)
            for (i = 0; i < 256; i++)
                levels[((long)t * 16 + v) * 256 + i] =
                    (unsigned char)(0x80 + (short)((long)pl_wave[t][i] * MEL_AMP * v / (15L * 127)));
    for (k = 0; k < 65536L; k++)
        btable[k] = (signed char)((short)pl_bwave[((k + 32768L) >> 8) & 255] * BAS_AMP / 127);
    mx.btab = btable + 32768L;
    silence_all();
    fill_hw(hw, 370);
    __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr));
    VIA_B &= 0x7F;                           /* PB7 = 0: sound on */
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr));
    ttask.tmAddr = (ProcPtr)snd_tm_glue;
    InsTime((QElemPtr)&ttask);
    tm_installed = 1;
    vtask.qType = vType;
    vtask.vblAddr = (ProcPtr)snd_vbl_glue;
    vtask.vblCount = 1;
    vtask.vblPhase = 0;
    if (VInstall(&vtask) == noErr)
        installed = 1;
    return installed ? 0 : -3;
}

void pl_quit(void)
{
    playing = 0;
    mixing = 0;
    if (installed) {
        VRemove(&vtask);
        installed = 0;
    }
    if (tm_installed) {
        RmvTime((QElemPtr)&ttask);
        tm_installed = 0;
    }
    if (hw)
        fill_hw(hw, 370);
}

void pl_reset(void)
{
    playing = 0;
    finished = 0;
    nmel = nbas = 0;
    ready = 0;
    cur_w = -1;
}

void pl_set_tempo(short bpm, short beat_ticks)
{
    /* ticks per frame = bpm * beat_ticks / (60 s * 60.15 Hz), in 16.16 */
    tpf = (unsigned long)((long)bpm * beat_ticks * 65536L / 3609L);
}

void pl_set_sound(short on) { sound_on = on; }
void pl_set_timbre(short t) { timbre = t; }
void pl_set_bass(short on) { bass_on = on; }

short pl_start(void)
{
    if (nmel == 0)
        return 0;
    playing = 0;
    t16 = 0;
    mi = bi = -1;
    mend = bend = mgate = 0;
    nframes = 0;
    playing = 1;
    return 1;
}

void pl_stop(void) { playing = 0; }
short pl_playing(void) { return playing; }
short pl_cur_note(void) { return playing ? cur_w : -1; }
long pl_ready_ticks(void) { return ready; }
long pl_pos_ticks(void) { return (long)(t16 >> 16); }
short pl_nmel(void) { return nmel; }
short pl_nbas(void) { return nbas; }

void pl_add_melody(short dur, unsigned char midi, short wnote)
{
    if (nmel >= PL_MAXP)
        return;
    mel[nmel].dur = dur;
    mel[nmel].midi = midi;
    mel[nmel].wnote = wnote;
    nmel++;
}

void pl_add_bass(short dur, unsigned char midi)
{
    if (nbas >= PL_MAXP)
        return;
    bas[nbas].dur = dur;
    bas[nbas].midi = midi;
    bas[nbas].wnote = -1;
    nbas++;
}

void pl_mark_ready(void)
{
    long t = 0;
    short i;
    for (i = 0; i < nmel; i++)
        t += mel[i].dur;
    ready = t;
}

void pl_copy(short m0, short m1, short b0, short b1)
{
    short i;
    for (i = m0; i < m1; i++)
        pl_add_melody(mel[i].dur, mel[i].midi, mel[i].wnote);
    for (i = b0; i < b1; i++)
        pl_add_bass(bas[i].dur, bas[i].midi);
}

void pl_finished(void)
{
    pl_mark_ready();
    finished = 1;
}

void pl_debug(unsigned long *out)
{
    out[0] = (unsigned long)nmel | ((unsigned long)nbas << 16);
    out[1] = (unsigned long)ready;
    out[2] = t16 >> 16;
    out[3] = (unsigned long)playing | ((unsigned long)mixing << 8) | ((unsigned long)level << 16);
    out[4] = (unsigned long)(unsigned short)mi | ((unsigned long)(unsigned short)bi << 16);
    out[5] = tpf;
}

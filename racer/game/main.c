/*
 * Endurance: an endurance racer for the Macintosh Plus (68000, 1 MB, System 6).
 *
 * Drive an endless road; each day has a quota of cars to pass, and a day runs through six
 * stages (morning, snow, dusk, night, fog, dawn). The road renderer and sprite blitter are
 * generated 68000 assembly (racer/engine, linked in from road_engine.s); this file is the
 * game: input, physics, traffic, day cycle, HUD, title and game over, best score.
 *
 * Controls: mouse moves the car left/right, mouse button accelerates. Keyboard: A / D steer,
 * W or Space accelerate, S brakes (arrows, X and Z also work), Return starts, P pauses,
 * M toggles sound, Esc (or Cmd-Q) goes back to the title, and quits from the title.
 *
 * Tests: a debug block tagged 'ENDU' (see Dbg) lets Lua scripts in MAME read the game state,
 * measure frame rate in emulated ticks, and send commands (start, autopilot, skip stage).
 */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Events.h>
#include <OSUtils.h>
#include <MacMemory.h>
#include <Files.h>
#include <TextUtils.h>
#include <string.h>
#include "road_engine.h"
#include "sprites.h"
#include "gamedata.h"

#define MUL16(a, b) ((long)(short)(a) * (short)(b))   /* 68000 muls.w, no library call */
static inline unsigned short udiv16(unsigned long a, unsigned short b)   /* divu.w: a/b < 65536 */
{
    __asm__("divu.w %1,%0" : "+d"(a) : "d"(b));
    return (unsigned short)a;
}
#define LM_W(a) (*(volatile short *)(a))
#define LM_B(a) (*(volatile unsigned char *)(a))

enum { M_TITLE, M_PLAY, M_OVER };

/* ------------------------------------------------------------------ tuning */
#define MAXSP       4608    /* top speed: 18 world units per tick (8.8 fixed) */
#define OFFMAX      1536    /* top speed on the grass */
#define ROADX       1024    /* lateral units: road edge at +-ROADX */
#define LANEX       640     /* lane centres at -LANEX, 0, +LANEX */
#define XLIM        1500
#define CARLEN      150     /* collision depth (world units) */
#define CARWX       330     /* same-lane test for rivals (lateral units) */
#define CARW        360     /* cars overlap when lateral distance < CARW (a car is ~387 wide) */
#define NRIV        8
#define ZPERKM10    2000    /* odometer: world units per 0.1 km */
#define COPY_TOP    (PF_HY + 1 - MTN_H)
#define BANNER_Y    (COPY_TOP - 16)  /* baseline of DAY n / GAME OVER (24 pt); hint line 13 px lower */
#define MTNW        57      /* words per row of an extended (wrapped) mountain strip */
#define NSEG        128
static const short stage_len[NSTAGES] = {3000, 2400, 1800, 2400, 1800, 1200};   /* ticks */

/* ------------------------------------------------------------------ debug block */
typedef struct {
    unsigned long magic, version, notmagic;     /*  0  4  8 */
    unsigned long frames;                       /* 12 frames drawn */
    unsigned long ticks;                        /* 16 TickCount at the last frame */
    short mode, day, quota, stage;              /* 20 22 24 26 */
    short speed, px;                            /* 28 30 */
    long odo10;                                 /* 32 distance, 0.1 km */
    unsigned long camz;                         /* 36 */
    short passed, gotpassed, crashes, fps10;    /* 40 42 44 46 */
    unsigned long seed;                         /* 48 tests may set before starting */
    short cmd, autopilot;                       /* 52 command from tests, 54 autopilot */
    long best;                                  /* 56 */
    short input, nriv;                          /* 60 input bits, 62 active rivals */
    short stageTick, offroad;                   /* 64 66 */
    unsigned long dayTick;                      /* 68 */
    short mdx, keys;                            /* 72 last mouse dx, 74 raw key bits */
    short hudDraws, stageChanges;               /* 76 78 */
    long saveErr;                               /* 80 */
    unsigned long prof[6];                      /* 84: ticks in geom, draw, sprites, copy+mtn, hud+snd, logic */
    unsigned long profFrames;                   /* 108 */
    short dayNet[8];                            /* 112: net cars passed in each finished day */
    short titleLineW, quits;                    /* 128: width of the controls line, 130: quit requests */
    unsigned long mainAddr;                     /* 132: run-time address of main() (PC profiling) */
    short sndOn, pad2;                          /* 136: sound on (M toggles) */
} Dbg;
static Dbg dbg;
enum { CMD_START = 1, CMD_NEXTSTAGE = 3, CMD_QUOTA0 = 5, CMD_ENDDAY = 6 };

/* ------------------------------------------------------------------ state */
static RoadState rs;
static unsigned char *buf;          /* offscreen playfield, PF_H x PF_RB */
static RoadRow *rows;
static unsigned char *scr;          /* screen address of playfield (0,0) */
static unsigned short *mtnbuf;      /* 16 shifts x MTN_H x MTNW words */
static long ring[NSEG * 2];
static short ring_w;
static unsigned long gen_end;
static short straight_next;
static unsigned long rnd_state = 0x2545F491;
static short mode, full_copy, mtn_dirty, last_mscroll = -1;
static PatSet psets[4], fogsets[2];
static unsigned char *pieces;            /* edge piece tables for the 4 pattern sets */
#define LSZ ((PF_RWMAX + 1) * 256L)
#define MSZ ((PF_MWMAX + 1) * 256L)
static short pf_left, pf_top;       /* playfield position in global coordinates */
static GrafPort scrPort, bufPort;

typedef struct { unsigned long z; short x, tx, speed, frac, kind, active, ahead, lanet; } Rival;
static Rival riv[NRIV];
static unsigned long pz;            /* player world z */
static short pzf, px, speed, crash, offroad, spawn_t;
static short day, stage, stage_tick, quota;
static long day_tick, odo10, best, odo_acc;
static long mscroll;                /* mountain scroll, 1/4096 px */
static short banner_t;              /* ticks left to show the day banner */
static unsigned long zstart;

/* ------------------------------------------------------------------ helpers */
static unsigned short rnd(void)
{
    unsigned long x = rnd_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rnd_state = x;
    return (unsigned short)(x >> 8);
}
static short rndn(short n) { return (short)(((unsigned long)rnd() * (unsigned short)n) >> 16); }
static short iabs(short v) { return v < 0 ? -v : v; }

static void pcat(unsigned char *d, const unsigned char *s)   /* Pascal string append */
{
    short i;
    for (i = 1; i <= s[0] && d[0] < 254; i++) d[++d[0]] = s[i];
}
static void pcatc(unsigned char *d, const char *s)
{
    while (*s && d[0] < 254) d[++d[0]] = (unsigned char)*s++;
}
static void pnum(unsigned char *d, long n)
{
    unsigned char t[16];
    NumToString(n, t);
    pcat(d, t);
}
static void pnum10(unsigned char *d, long n)     /* n/10 with one decimal */
{
    pnum(d, n / 10);
    pcatc(d, ".");
    pnum(d, n % 10);
}

/* ------------------------------------------------------------------ track */
static void track_add(long len, long ddx)
{
    ring[ring_w * 2] = len;
    ring[ring_w * 2 + 1] = ddx;
    ring_w = (ring_w + 1) & (NSEG - 1);
    gen_end += len;
}

static void track_gen(void)
{
    while (gen_end < rs.camz + 80000UL) {
        if (straight_next) {
            track_add(1200 + rndn(3500), 0);
        } else {
            short maxc = 300 + (day > 4 ? 4 : day) * 90;
            long c = 120 + rndn(maxc);
            long len = 1500 + rndn(4500);
            if (rndn(2)) c = -c;
            track_add(500, c / 2);
            track_add(len, c);
            track_add(500, c / 2);
        }
        straight_next = !straight_next;
    }
}

static void track_reset(void)
{
    ring_w = 0;
    gen_end = 0;
    straight_next = 0;
    track_add(5000, 0);
    rs.ring = ring;
    rs.ringend = ring + NSEG * 2;
    rs.segp = ring;
    rs.segend = ring[0];
    rs.camz = 0;
    track_gen();
}

static void cam_set(unsigned long camz)
{
    unsigned long zb = camz + PF_ZNEAR;
    rs.camz = camz;
    while (zb >= rs.segend) {
        rs.segp += 2;
        if (rs.segp >= rs.ringend) rs.segp = rs.ring;
        rs.segend += rs.segp[0];
    }
}

/* ------------------------------------------------------------------ stages, sky, mountains */
static void mtn_build(short st)
{
    const unsigned char *strip = mtn_strips[st];
    unsigned short e[MTNW + 1];
    short r, j, v;
    for (r = 0; r < MTN_H; r++) {
        const unsigned char *s = strip + r * 64;
        for (j = 0; j <= MTNW; j++) {
            short b = (j * 2) & 63;
            e[j] = (unsigned short)((s[b] << 8) | s[b + 1]);
        }
        for (v = 0; v < 16; v++) {
            unsigned short *o = mtnbuf + ((long)v * MTN_H + r) * MTNW;
            if (v == 0) {
                for (j = 0; j < MTNW; j++) o[j] = e[j];
            } else {
                for (j = 0; j < MTNW; j++) o[j] = (unsigned short)((e[j] << v) | (e[j + 1] >> (16 - v)));
            }
        }
    }
}

static void sky_fill(void)
{
    short y, i;
    for (y = 0; y < COPY_TOP; y++) {
        unsigned long p = sky_pats[stage][y & 1];
        unsigned long *d = (unsigned long *)(buf + (long)y * PF_RBV + PF_MG * 4);
        for (i = 0; i < PF_RB / 4; i++) *d++ = p;
    }
}

static unsigned long range_mask(short s, short e)   /* bits [s, e) of a long, MSB = bit 0 */
{
    if (s < 0) s = 0;
    if (e > 32) e = 32;
    if (s >= e) return 0;
    return (0xFFFFFFFFUL >> s) & (e == 32 ? 0xFFFFFFFFUL : ~(0xFFFFFFFFUL >> e));
}

static void build_pieces(unsigned char *t, short wmax, unsigned long p0, unsigned long p1, unsigned long p2)
{
    short w, a, i;
    unsigned long *o = (unsigned long *)t;
    for (w = 0; w <= wmax; w++)
        for (a = 0; a < 32; a++)
            for (i = 0; i < 2; i++) {
                unsigned long m0 = range_mask(-1, a - i * 32);
                unsigned long m1 = range_mask(a - i * 32, a + w - i * 32);
                *o++ = (p0 & m0) | (p1 & m1) | (p2 & ~(m0 | m1));
            }
}

static void build_sets(short st)
{
    short n;
    const unsigned long *src = stage_pats[st];
    for (n = 0; n < 4; n++) {
        PatSet *ps = &psets[n];
        unsigned char *t = pieces + n * (2 * LSZ + MSZ);
        unsigned long G = src[n * 4], R = src[n * 4 + 1], A = src[n * 4 + 2], M = src[n * 4 + 3];
        ps->pat[0] = G; ps->pat[1] = R; ps->pat[2] = A; ps->pat[3] = M;
        ps->pl = t; ps->pr = t + LSZ; ps->pm = t + 2 * LSZ;
        build_pieces(t, PF_RWMAX, G, R, A);
        build_pieces(t + LSZ, PF_RWMAX, A, R, G);
        build_pieces(t + 2 * LSZ, PF_MWMAX, A, M, A);
    }
    for (n = 0; n < 2; n++)
        for (st = 0; st < 4; st++) fogsets[n].pat[st] = fog_pats[n * 4 + st];
}

static void draw_sprite(short idx, short cx, short ybot, short cliptop);
static void title_art(void);
static void banner_text(const unsigned char *s);

static void set_stage(short st)
{
    stage = st;
    stage_tick = 0;
    build_sets(st);
    rs.pats = psets;
    mtn_build(st);
    mtn_dirty = 1;
    sky_fill();
    if (mode == M_TITLE) title_art();
    else if (banner_t > 0) {
        unsigned char t[32];
        t[0] = 0;
        pcatc(t, "DAY ");
        pnum(t, day);
        banner_text(t);
    }
    full_copy = 1;
    dbg.stageChanges++;
}

static __attribute__((noinline)) void draw_mtn(void)    /* the mountain band goes straight to the screen, only when it moves */
{
    short s = (short)((mscroll >> 12) & 511);
    const unsigned short *src = mtnbuf + (long)(s & 15) * MTN_H * MTNW + (s >> 4);
    if (s == last_mscroll && !mtn_dirty) return;
    last_mscroll = s;
    mtn_dirty = 0;
    RowCopy(src, scr + (long)COPY_TOP * 64, MTN_H, MTNW * 2);
}

/* ------------------------------------------------------------------ sprites */
static void draw_sprite(short idx, short cx, short ybot, short cliptop)
{
    const SprInfo *s = &spr_info[idx];
    short w = s->w, h = s->h, wpr = s->wpr, ns = s->nshift;
    short x = cx - (w >> 1), y = ybot - h + 1;
    short col0 = x >> 4, si = (short)((x & 15) * ns >> 4);
    short r0 = 0, r1 = h, c0 = 0, c1 = wpr;
    const unsigned char *src;
    unsigned char *dst;
    if (y < cliptop) r0 = cliptop - y;
    if (y + h > PF_H) r1 = PF_H - y;
    if (col0 < -PF_MG * 2) c0 = -PF_MG * 2 - col0;
    if (col0 + wpr > PF_W / 16 + PF_MG * 2) c1 = PF_W / 16 + PF_MG * 2 - col0;
    if (r0 >= r1 || c0 >= c1) return;
    src = spr_data + s->off + ((long)si * h + r0) * wpr * 4;
    dst = buf + (long)(y + r0) * PF_RBV + PF_MG * 4 + col0 * 2;
    if (c0 == 0 && c1 == wpr) {
        SprBlit(dst, src, r1 - r0, wpr, PF_RBV - wpr * 2);
    } else {    /* clipped at the buffer margin (rare): word by word from the paired layout */
        short r, c;
        for (r = r0; r < r1; r++, src += wpr * 4, dst += PF_RBV)
            for (c = c0; c < c1; c++) {
                const unsigned short *q = (const unsigned short *)src;
                unsigned short m, im;
                if ((wpr & 1) && c == wpr - 1) { m = q[(c >> 1) * 4]; im = q[(c >> 1) * 4 + 1]; }
                else { m = q[(c >> 1) * 4 + (c & 1)]; im = q[(c >> 1) * 4 + 2 + (c & 1)]; }
                ((unsigned short *)dst)[c] = (((unsigned short *)dst)[c] & m) | im;
            }
    }
}

static __attribute__((noinline)) void draw_rivals(void)
{
    short order[NRIV], n = 0, i, j;
    short fog_k = stage_info[stage].fog_k, night = stage_info[stage].night;
    for (i = 0; i < NRIV; i++)
        if (riv[i].active) order[n++] = i;
    for (i = 1; i < n; i++) {           /* farthest first */
        short t = order[i];
        for (j = i; j > 0 && riv[order[j - 1]].z < riv[t].z; j--) order[j] = order[j - 1];
        order[j] = t;
    }
    for (i = 0; i < n; i++) {
        Rival *r = &riv[order[i]];
        long dz = (long)(r->z - rs.camz);
        short k, sz, cx;
        if (dz <= (long)rt_zrow_c[PF_KB] || dz >= 65000) continue;
        k = (short)udiv16(PF_ZS, (unsigned short)dz);
        if (k < 1 || k > PF_KB) continue;
        if (fog_k && k < fog_k) continue;
        sz = rival_size[k];
        if (sz < 0) continue;
        cx = (rows[k].cx4 >> 2) + (short)(MUL16(r->x, rt_hw_c[k]) >> 10);
        draw_sprite((night ? SPR_TAIL0 : SPR_RIVAL0 + r->kind * NRIVAL_SIZES) + sz, cx, PF_HY + k, PF_HY + 1);
    }
}

static __attribute__((noinline)) void draw_player(void)
{
    short cx = (rows[KP].cx4 >> 2) + (short)(((long)px * rt_hw_c[KP]) >> 10);
    short y = PF_HY + KP, idx = SPR_P_STRAIGHT;
    if (crash > 0) idx = SPR_P_CRASH;
    else if (dbg.input & 4) idx = SPR_P_LEFT;
    else if (dbg.input & 8) idx = SPR_P_RIGHT;
    if (offroad && speed > 256) y -= (short)(dbg.frames & 1);
    draw_sprite(idx, cx, y, PF_HY + 1);
}

/* ------------------------------------------------------------------ text */
static void banner_text(const unsigned char *s)
{
    short w;
    SetPort(&bufPort);
    TextFont(0);
    TextSize(24);
    TextFace(bold);
    w = StringWidth(s);
    TextMode(srcOr);
    {   /* white halo, then black text */
        short dx, dy;
        TextMode(srcBic);
        for (dy = -2; dy <= 2; dy += 2)
            for (dx = -2; dx <= 2; dx += 2) {
                MoveTo((PF_W - w) / 2 + dx, BANNER_Y + dy);
                DrawString(s);
            }
        TextMode(srcOr);
        MoveTo((PF_W - w) / 2, BANNER_Y);
        DrawString(s);
    }
    TextSize(0);
    TextFace(0);
}

static void title_art(void)
{
    draw_sprite(SPR_LOGO, PF_W / 2, COPY_TOP - 1, 0);    /* just above the mountain band */
}

static short hud_cache[6];
static __attribute__((noinline)) void hud_field(short x, short w, const unsigned char *s)
{
    Rect r;
    SetPort(&scrPort);
    r.left = pf_left + x;
    r.right = r.left + w;
    r.top = pf_top + PF_H + 4;
    r.bottom = r.top + 22;
    PaintRect(&r);
    TextFont(0);
    TextSize(0);
    TextMode(srcBic);
    MoveTo(r.left + 6, r.top + 16);
    DrawString(s);
    dbg.hudDraws++;
}

static void hud_line2(const char *const *alts)   /* second line under the HUD (title only).
    alts: candidates in order of preference, NULL-terminated, each starting with 'C' (Chicago 12)
    or 'G' (ROM Geneva 9); draws the first that fits the playfield. NULL erases the line. */
{
    Rect r;
    unsigned char t[100];
    short w = 0, a, f = 0;
    SetPort(&scrPort);
    r.left = pf_left;
    r.right = pf_left + PF_W;
    r.top = pf_top + PF_H + 27;
    r.bottom = r.top + 16;
    PaintRect(&r);
    if (!alts) return;
    for (a = 0; alts[a]; a++) {
        f = alts[a][0] == 'G';
        t[0] = 0;
        pcatc(t, alts[a] + 1);
        TextFont(f ? 3 : 0);
        TextSize(f ? 9 : 0);
        w = StringWidth(t);
        if (w <= PF_W - 12) break;
    }
    dbg.titleLineW = w;
    TextMode(srcBic);
    MoveTo(pf_left + (PF_W - w) / 2, r.top + 12);
    DrawString(t);
    TextFont(0);
    TextSize(0);
}

static const char *const controls_line[] = {
    "CMouse or A/D steer - button or W gas - S brake - P pause - M sound - Esc quit",
    "GMouse or A/D steer  -  button or W gas  -  S brake  -  P pause  -  M sound  -  Esc quit",
    "GMouse or A/D steer - button or W gas - S brake - P pause - M sound - Esc quit",
    "GMouse or A/D steer - W or button gas - S brake - P pause - M sound - Esc quit",
    "GMouse/A/D steer - W/button gas - S brake - P pause - M sound - Esc quit",
    "GMouse/A/D steer  W/button gas  S brake  P pause  M sound  Esc quit",
    "CA/D steer  W gas  S brake  P pause  M sound  Esc quit",
    0
};

static void hud_clear(void)
{
    short i;
    for (i = 0; i < 6; i++) hud_cache[i] = -32000;
}

static __attribute__((noinline)) void hud_update(void)
{
    unsigned char t[64];
    short sp = (short)((MUL16(speed, 711) + 8192) >> 14);     /* km/h, rounded: 200 at MAXSP */
    if (mode == M_TITLE) {
        if (hud_cache[0] != 1000) {
            hud_cache[0] = 1000;
            t[0] = 0;
            pcatc(t, "Click or press Return to race");
            hud_field(0, 250, t);
            t[0] = 0;
            pcatc(t, "BEST ");
            pnum10(t, best);
            pcatc(t, " km");
            hud_field(250, 134, t);
            hud_line2(controls_line);
        }
        return;
    }
    if (hud_cache[0] != day) {
        hud_cache[0] = day;
        t[0] = 0;
        pcatc(t, "DAY ");
        pnum(t, day);
        hud_field(0, 70, t);
    }
    if (hud_cache[1] != quota) {
        hud_cache[1] = quota;
        t[0] = 0;
        if (quota > 0) {
            pcatc(t, "CARS ");
            pnum(t, quota);
        } else {
            pcatc(t, "QUOTA MET!");
        }
        hud_field(70, 110, t);
    }
    if (hud_cache[2] != sp && ((dbg.frames & 15) == 0 || sp == 0)) {
        hud_cache[2] = sp;
        t[0] = 0;
        pnum(t, sp);
        pcatc(t, " km/h");
        hud_field(180, 90, t);
    }
    if (hud_cache[3] != (short)odo10) {
        hud_cache[3] = (short)odo10;
        t[0] = 0;
        pnum10(t, odo10);
        pcatc(t, " km");
        hud_field(270, 114, t);
    }
}

/* ------------------------------------------------------------------ sound
 * The Plus plays its 370-word sound buffer (sample = high byte of each word, at 22.25 kHz, one
 * buffer per video frame) by DMA. We write waveforms straight into it, and only when the sound
 * changes (engine pitch step, effect start/end), so steady sound costs no CPU. The waveform
 * restarts every frame, which adds a 60 Hz buzz: fine for an engine. */
#define VIA_B (*(volatile unsigned char *)0xEFE1FE)
#define SND_N 370
static unsigned char *sndbuf;
static short snd_on = 1, snd_period = -1, snd_fx, snd_fx_t, snd_last_fx = -1;
enum { FX_NONE, FX_CRASH, FX_PASS, FX_PASSED };

static void snd_enable(short on)
{
    unsigned short sr;
    __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr));
    if (on) VIA_B &= 0x7F; else VIA_B |= 0x80;
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr));
}

static __attribute__((noinline)) void snd_fill(short period, short amp, short fx)
{
    unsigned char *p = sndbuf;
    short i, ph = 0, step, v, fph = 0;
    static unsigned short nz = 1;     /* own noise generator: sound must not disturb the game's */
    if (!sndbuf) return;
    if (period < 8) period = 8;
    step = (short)((amp * 2 * 256L) / period);
    v = (128 - amp) << 8;
    for (i = 0; i < SND_N; i++, p += 2) {            /* sawtooth engine */
        short out = v >> 8;
        v += step;
        if (++ph >= period) { ph = 0; v = (128 - amp) << 8; }
        if (fx == FX_CRASH) { nz = nz * 25173 + 13849; out += (short)((nz >> 8) & 63) - 32; }
        else if (fx == FX_PASS) { out += (fph & 8) ? 22 : -22; fph++; }
        else if (fx == FX_PASSED) { out += (fph & 32) ? 18 : -18; fph++; }
        *p = (unsigned char)out;
    }
}

static void snd_silence(void)
{
    short i;
    if (!sndbuf) return;
    for (i = 0; i < SND_N; i++) sndbuf[i * 2] = 128;
    snd_period = -1;
    snd_last_fx = -1;
}

static void snd_effect(short fx, short ticks)
{
    if (fx >= snd_fx || snd_fx_t == 0) { snd_fx = fx; snd_fx_t = ticks; }
}

static __attribute__((noinline)) void snd_update(void)         /* once per frame */
{
    short period, fx;
    if (!sndbuf) return;
    if (!snd_on || mode == M_OVER) {
        if (snd_period != -2) { snd_silence(); snd_period = -2; }
        return;
    }
    fx = snd_fx_t > 0 ? snd_fx : FX_NONE;
    period = 300 - (short)(MUL16(speed, 747) >> 14);   /* 74 Hz idle .. 247 Hz flat out */
    period &= ~15;          /* 14 pitch steps: the buffer is rewritten only when the step changes */
    if (period != snd_period || fx != snd_last_fx) {
        snd_fill(period, mode == M_TITLE ? 6 : 14, fx);
        snd_period = period;
        snd_last_fx = fx;
    }
}

/* ------------------------------------------------------------------ best score file */
static unsigned char best_name[] = "\pEndurance Best";
static void load_best(void)
{
    ParamBlockRec pb;
    long v = 0;
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioNamePtr = best_name;
    pb.ioParam.ioPermssn = 1;
    if (PBOpenSync(&pb) != 0) return;
    {
        short ref = pb.ioParam.ioRefNum;
        memset(&pb, 0, sizeof pb);
        pb.ioParam.ioRefNum = ref;
        pb.ioParam.ioBuffer = (Ptr)&v;
        pb.ioParam.ioReqCount = 4;
        pb.ioParam.ioPosMode = 1;
        if (PBReadSync(&pb) == 0 && v > 0 && v < 10000000) best = v;
        memset(&pb, 0, sizeof pb);
        pb.ioParam.ioRefNum = ref;
        PBCloseSync(&pb);
    }
}

static void save_best(void)
{
    ParamBlockRec pb;
    long v = best;
    short ref, err;
    memset(&pb, 0, sizeof pb);
    pb.fileParam.ioNamePtr = best_name;
    PBCreateSync(&pb);
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioNamePtr = best_name;
    pb.ioParam.ioPermssn = 2;
    err = PBOpenSync(&pb);
    if (err) { dbg.saveErr = err; return; }
    ref = pb.ioParam.ioRefNum;
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioRefNum = ref;
    pb.ioParam.ioBuffer = (Ptr)&v;
    pb.ioParam.ioReqCount = 4;
    pb.ioParam.ioPosMode = 1;
    err = PBWriteSync(&pb);
    dbg.saveErr = err;
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioRefNum = ref;
    PBCloseSync(&pb);
    memset(&pb, 0, sizeof pb);
    PBFlushVolSync(&pb);
}

/* ------------------------------------------------------------------ input */
enum { IN_ACC = 1, IN_BRAKE = 2, IN_LEFT = 4, IN_RIGHT = 8, IN_START = 16, IN_MUTE = 32, IN_BTN = 64, IN_PAUSE = 128,
       IN_QUIT = 256 };
static short mouse_cx, mouse_cy;

static __attribute__((noinline)) short read_input(short *mdx)
{
    unsigned char km[16];
    short in = 0, h;
#define KEY(c) ((km[(c) >> 3] >> ((c) & 7)) & 1)
    GetKeys(km);
    /* standard keys (DECISIONS.md): A/D steer, W or Space gas, S brake; the old keys still work:
       arrows (Plus 0x46/0x42/0x4D/0x48, ADB 0x7B/0x7C/0x7E/0x7D), X gas, Z brake */
    if (KEY(0x00) || KEY(0x46) || KEY(0x7B)) in |= IN_LEFT;
    if (KEY(0x02) || KEY(0x42) || KEY(0x7C)) in |= IN_RIGHT;
    if (KEY(0x0D) || KEY(0x31) || KEY(0x07) || KEY(0x4D) || KEY(0x7E)) in |= IN_ACC;
    if (KEY(0x01) || KEY(0x06) || KEY(0x48) || KEY(0x7D)) in |= IN_BRAKE;
    if (KEY(0x24) || KEY(0x4C)) in |= IN_START;
    if (KEY(0x2E)) in |= IN_MUTE;                 /* M: sound on/off */
    if (KEY(0x23)) in |= IN_PAUSE;
    /* Esc: 0x35 on ADB keyboards (SE); the Plus has none, emu/play.sh maps the host's Esc to its
       backquote key, 0x32. Cmd-Q still works. */
    if (KEY(0x35) || KEY(0x32) || (KEY(0x37) && KEY(0x0C))) in |= IN_QUIT;
    dbg.keys = in;
    if (Button()) in |= IN_ACC | IN_BTN;
    /* mouse: horizontal movement since the last frame, then re-centre */
    h = LM_W(0x82A);
    *mdx = h - mouse_cx;
    LM_W(0x828) = mouse_cy; LM_W(0x82A) = mouse_cx;
    LM_W(0x82C) = mouse_cy; LM_W(0x82E) = mouse_cx;
    LM_B(0x8CE) = LM_B(0x8CF);
    return in;
}

/* ------------------------------------------------------------------ autopilot (attract mode, tests) */
static __attribute__((noinline)) short lane_clear(short lx)
{
    short i, best_d = 32000;
    for (i = 0; i < NRIV; i++) {
        long d;
        if (!riv[i].active || iabs(riv[i].x - lx) > CARW + 60) continue;
        d = (long)(riv[i].z - pz);
        if (d > -CARLEN && d < best_d) best_d = (short)(d < 0 ? 0 : d);
    }
    return best_d;
}

static __attribute__((noinline)) short autopilot(short *steer)
{
    static short target;
    short cur = lane_clear(px);
    short lanes[3] = {-LANEX, 0, LANEX}, i, err;
    if (cur < 2000) {
        short bestc = lane_clear(target);
        for (i = 0; i < 3; i++) {
            short c = lane_clear(lanes[i]);
            if (c > bestc + 300) { bestc = c; target = lanes[i]; }
        }
    }
    if (target < -LANEX || target > LANEX) target = 0;
    err = target - px;
    *steer = (short)(MUL16(err, 43) >> 8);     /* err / 6 */
    if (*steer > 40) *steer = 40;
    if (*steer < -40) *steer = -40;
    return (cur < 350 && lane_clear(target) < 350) ? IN_BRAKE : IN_ACC;
}

/* ------------------------------------------------------------------ game logic (one tick) */
static short quota_for(short d)     /* 50, 70, then +15 a day up to 150 */
{
    short q = d == 1 ? 50 : 70 + 15 * (d - 2);
    return q > 150 ? 150 : q;
}

static void new_game(void)
{
    short i;
    rnd_state = dbg.seed ? dbg.seed : (TickCount() | 1);
    for (i = 0; i < NRIV; i++) riv[i].active = 0;
    pz = 1000; pzf = 0; px = 0; speed = 0; crash = 0; offroad = 0; spawn_t = 60;
    day = 1; quota = quota_for(1); day_tick = 0; odo10 = 0; odo_acc = 0; mscroll = 0;
    dbg.passed = dbg.gotpassed = dbg.crashes = 0;
    track_reset();
    zstart = pz;
    mode = M_PLAY;
    banner_t = 150;
    hud_clear();
    hud_line2(0);
    set_stage(0);
}

static void spawn_rival(void)
{
    short i, j, lane;
    Rival *r = 0;
    for (i = 0; i < NRIV; i++)
        if (!riv[i].active) { r = &riv[i]; break; }
    if (!r) return;
    r->kind = rndn(3);
    {
        short top = 46 + (day > 6 ? 6 : day) * 4;          /* rivals: 25% .. top% of max speed */
        r->speed = (short)((long)MAXSP * (25 + rndn(top - 25)) / 100);
    }
    lane = rndn(3) - 1;
    r->x = r->tx = lane * LANEX;
    r->lanet = 120 + rndn(400);
    if (speed > r->speed) r->z = pz + 3500 + rndn(2000);
    else r->z = pz - 1600 - rndn(400);
    for (j = 0; j < NRIV; j++) {                       /* keep spawns apart */
        long d;
        if (!riv[j].active) continue;
        d = (long)(riv[j].z - r->z);
        if (d > -1500 && d < 1500 && iabs(riv[j].x - r->x) < CARW) return;
    }
    r->frac = 0;
    r->ahead = (long)(r->z - pz) > 0;
    r->active = 1;
}

static short lane_free(short lx, unsigned long z, short self)
{
    short j;
    for (j = 0; j < NRIV; j++) {
        long g;
        if (j == self || !riv[j].active || iabs(riv[j].tx - lx) > CARWX) continue;
        g = (long)(riv[j].z - z);
        if (g > -1200 && g < 1200) return 0;
    }
    return 1;
}

static unsigned short riv_tick;
static void rivals_tick(void)
{
    short i, j, n = 0;
    riv_tick++;
    for (i = 0; i < NRIV; i++) n += riv[i].active;
    if (--spawn_t <= 0 && n < 5 + (day > 4 ? 2 : day / 2)) {
        short base = 48 - (day > 6 ? 6 : day) * 3;
        spawn_rival();
        spawn_t = base + rndn(base);
    }
    n = 0;
    for (i = 0; i < NRIV; i++) {
        Rival *r = &riv[i];
        long d;
        if (!r->active) continue;
        n++;
        r->frac += r->speed;
        r->z += r->frac >> 8;
        r->frac &= 255;
        /* lane changes: occasionally, and to avoid a slower car ahead in the same lane */
        if (--r->lanet <= 0) {
            r->lanet = 200 + rndn(500);
            if (day > 1 && rndn(3) == 0) r->tx = (rndn(3) - 1) * LANEX;
        }
        if (((riv_tick + i) & 3) == 0 && r->x == r->tx)   /* every 4th tick, not mid lane change */
        for (j = 0; j < NRIV; j++) {      /* catching up with a slower car: move to a free lane */
            Rival *o = &riv[j];
            long g;
            if (j == i || !o->active || iabs(o->tx - r->tx) > CARWX) continue;
            g = (long)(o->z - r->z);
            if (g > 0 && g < 900 && r->speed > o->speed) {
                short alt = r->tx == 0 ? (rndn(2) ? LANEX : -LANEX) : 0;
                if (lane_free(alt, r->z, i)) r->tx = alt;
                else if (r->tx == 0 && lane_free(-alt, r->z, i)) r->tx = -alt;
                break;
            }
        }
        if (r->x < r->tx) r->x += 6;
        if (r->x > r->tx) r->x -= 6;
        d = (long)(r->z - pz);
        /* collision */
        if (crash == 0 && d > -CARLEN && d < CARLEN && iabs(r->x - px) < CARW) {
            crash = 50;
            dbg.crashes++;
            snd_effect(FX_CRASH, 24);
            speed = r->speed / 2;
            pz = r->z - CARLEN - 20;
            d = (long)(r->z - pz);
        }
        /* passing */
        {
            short a = d > 0;
            if (a != r->ahead) {
                if (r->ahead) { quota--; dbg.passed++; snd_effect(FX_PASS, 5); }
                else { quota++; dbg.gotpassed++; snd_effect(FX_PASSED, 8); }
                r->ahead = a;
            }
        }
        if (d < -2000 || d > 20000) r->active = 0;
    }
    dbg.nriv = n;
}

static void end_day(void)
{
    if (day >= 1 && day <= 8) dbg.dayNet[day - 1] = quota_for(day) - quota;
    if (quota <= 0) {
        day++;
        quota = quota_for(day);
        banner_t = 180;
        hud_clear();
        set_stage(0);
    } else {
        unsigned char t[32];
        mode = M_OVER;
        banner_t = 0;
        if (odo10 > best) { best = odo10; save_best(); }
        dbg.best = best;
        sky_fill();
        t[0] = 0;
        pcatc(t, "GAME OVER");
        banner_text(t);
        {   /* hint line under the banner */
            unsigned char h[48];
            short w;
            h[0] = 0;
            pcatc(h, "Return: race again    Click: title");
            SetPort(&bufPort);
            TextFont(0);
            TextSize(0);
            w = StringWidth(h);
            TextMode(srcOr);
            MoveTo((PF_W - w) / 2, BANNER_Y + 13);     /* descenders stay above the mountain band */
            DrawString(h);
        }
        full_copy = 1;
        hud_clear();
    }
}

static void tick(short in, short mdx)
{
    short steer = 0, rate;
    long ddx = rs.segp[1];
    short grip = stage_info[stage].grip;
    if (mode == M_TITLE || dbg.autopilot) {
        in = (in & IN_START) | autopilot(&steer);
        mdx = 0;
    }
    if (mode == M_OVER) in = IN_BRAKE;
    if (snd_fx_t > 0) snd_fx_t--;
    dbg.input = in;
    /* speed */
    if (crash > 0) {
        crash--;
        speed -= speed >> 6;
    } else if (in & IN_ACC) {
        speed += ((MAXSP - speed) >> 7) + 4;
    } else if (in & IN_BRAKE) {
        speed -= 48;
    } else {
        speed -= 6;
    }
    offroad = px < -ROADX - 120 || px > ROADX + 120;
    if (offroad && speed > OFFMAX) speed -= 40;
    if (speed < 0) speed = 0;
    if (speed > MAXSP) speed = MAXSP;
    /* steering: keys or mouse, scaled down at low speed; snow lowers grip */
    rate = 8 + (speed >> 8);
    if (in & IN_LEFT) steer -= rate;
    if (in & IN_RIGHT) steer += rate;
    steer += mdx * 6;
    if (speed < 512) steer = (short)(MUL16(steer, speed) >> 9);
    if (crash > 0) steer = 0;
    px += (short)(MUL16(steer, grip + 256) >> 9);
    /* centrifugal drift on curves: outward, grows with speed squared */
    {
        short sp = speed >> 8;
        long drift = MUL16(ddx, sp * sp) >> 14;
        if (grip < 256) drift <<= 1;             /* snow: grip 128 doubles the drift */
        px -= (short)drift;
    }
    if (px < -XLIM) px = -XLIM;
    if (px > XLIM) px = XLIM;
    /* move */
    pzf += speed;
    pz += pzf >> 8;
    odo_acc += pzf >> 8;
    while (odo_acc >= ZPERKM10) { odo_acc -= ZPERKM10; odo10++; }
    pzf &= 255;
    mscroll += MUL16(ddx, speed >> 6);
    cam_set(pz - rt_zrow_c[KP]);
    rs.camx = (short)(((long)px * rt_hw_c[PF_KB]) >> 11);   /* camera follows the car halfway */
    track_gen();
    if (mode == M_PLAY) {
        rivals_tick();
        day_tick++;
        if (banner_t > 0 && --banner_t == 0) { sky_fill(); full_copy = 1; }
        if (++stage_tick >= stage_len[stage]) {
            if (stage + 1 < NSTAGES) set_stage(stage + 1);
            else end_day();
        }
    } else if (mode == M_TITLE) {
        rivals_tick();
    }
}

/* Quit: silence and disable sound, give the cursor back, and leave. On the game floppy the
   boot blocks name Endurance as the shell, so ExitToShell starts it again from the title. */
static void quit_app(void)
{
    snd_silence();
    snd_enable(0);
    FlushEvents(everyEvent, 0);
    ShowCursor();
    ExitToShell();
}

/* ------------------------------------------------------------------ frame */
static __attribute__((noinline)) void render(void)
{
    short k;
    short fog_k = stage_info[stage].fog_k;
    unsigned long t0 = TickCount(), t1;
    RoadGeom(&rs);
    t1 = TickCount(); dbg.prof[0] += t1 - t0; t0 = t1;
    for (k = 1; k < fog_k; k++) {
        rows[k].pat = &fogsets[(PF_HY + k) & 1];
        rows[k].marked = 2;
    }
    RoadDraw(&rs, buf + (PF_HY + 1) * (long)PF_RBV, 0);
    t1 = TickCount(); dbg.prof[1] += t1 - t0; t0 = t1;
    draw_rivals();
    draw_player();
    t1 = TickCount(); dbg.prof[2] += t1 - t0; t0 = t1;
    if (full_copy) {
        PfCopy(buf + PF_MG * 4, scr, COPY_TOP);
        mtn_dirty = 1;
        full_copy = 0;
    }
    draw_mtn();
    PfCopy(buf + (PF_HY + 1) * (long)PF_RBV + PF_MG * 4, scr + (PF_HY + 1) * 64L, PF_H - PF_HY - 1);
    t1 = TickCount(); dbg.prof[3] += t1 - t0; t0 = t1;
    hud_update();
    snd_update();
    dbg.prof[4] += TickCount() - t0;
    dbg.profFrames++;
}

static void fill_screen_black(void)
{
    Rect r = qd.screenBits.bounds;
    SetPort(&scrPort);
    PaintRect(&r);
}

int main(void)
{
    unsigned long last, fps_t;
    unsigned long fps_f = 0;
    BitMap bm;

    dbg.magic = 0x454E4455UL;      /* 'ENDU' */
    dbg.version = 1;
    dbg.notmagic = ~0x454E4455UL;
    dbg.mainAddr = (unsigned long)&main;
    dbg.sndOn = 1;
    MaxApplZone();
    InitGraf(&qd.thePort);
    InitFonts();
    FlushEvents(everyEvent, 0);
    InitCursor();
    HideCursor();
    OpenPort(&scrPort);
    OpenPort(&bufPort);

    {   /* layout: playfield + HUD strip, centred in screenBits.bounds */
        Rect b = qd.screenBits.bounds;
        short x = b.left + ((b.right - b.left) - PF_W) / 2;
        short y = b.top + ((b.bottom - b.top) - (PF_H + 30)) / 2;
        pf_left = x & ~15;
        pf_top = y;
        scr = (unsigned char *)qd.screenBits.baseAddr + (long)(pf_top - b.top) * qd.screenBits.rowBytes
              + ((pf_left - b.left) >> 3);
        mouse_cx = (b.left + b.right) / 2;
        mouse_cy = (b.top + b.bottom) / 2;
    }
    buf = (unsigned char *)NewPtrClear((long)PF_H * PF_RBV);
    pieces = (unsigned char *)NewPtr(4 * (2 * LSZ + MSZ));
    rows = (RoadRow *)NewPtrClear(8L * (PF_KB + 2));     /* + sentinel */
    mtnbuf = (unsigned short *)NewPtr(16L * MTN_H * MTNW * 2);
    if (!buf || !rows || !mtnbuf || !pieces) {
        dbg.mode = -1;
        for (;;) ;
    }
    bm.baseAddr = (Ptr)buf;
    bm.rowBytes = PF_RBV;
    SetRect(&bm.bounds, -PF_MG * 32, 0, PF_W + PF_MG * 32, PF_H);
    SetPort(&bufPort);
    SetPortBits(&bm);
    PortSize(PF_W, PF_H);
    RectRgn(bufPort.visRgn, &bm.bounds);
    RectRgn(bufPort.clipRgn, &bm.bounds);

    fill_screen_black();
    sndbuf = *(unsigned char **)0x266;
    snd_silence();
    snd_enable(1);
    load_best();
    dbg.best = best;
    rs.rows = rows;
    /* title: attract mode on the morning road */
    rnd_state = 12345;
    track_reset();
    pz = 1000;
    cam_set(pz - rt_zrow_c[KP]);
    mode = M_TITLE;
    hud_clear();
    set_stage(0);
    last = fps_t = TickCount();
    for (;;) {
        unsigned long now = TickCount();
        short n = (short)(now - last), mdx, in;
        if (n <= 0) continue;
        if (n > 4) n = 4;
        last = now;
        in = read_input(&mdx);
        dbg.mdx = mdx;
        if (dbg.cmd) {
            short c = dbg.cmd;
            dbg.cmd = 0;
            if (c == CMD_START) in |= IN_START;
            if (c == CMD_NEXTSTAGE && mode == M_PLAY) {
                if (stage + 1 < NSTAGES) set_stage(stage + 1); else end_day();
            }
            if (c == CMD_QUOTA0) quota = 0;
            if (c == CMD_ENDDAY && mode == M_PLAY) end_day();
        }
        {
            static short prev_in;
            short pressed = in & ~prev_in;      /* newly pressed this frame */
            prev_in = in;
            if (pressed & IN_QUIT) {
                dbg.quits++;
                if (mode == M_TITLE) quit_app();
                if (mode == M_PLAY && odo10 > best) { best = odo10; save_best(); dbg.best = best; }
                mode = M_TITLE;             /* during a race: back to the title */
                hud_clear();
                set_stage(0);
                continue;
            }
            if (mode == M_TITLE && (pressed & (IN_START | IN_BTN))) {
                new_game();
                in &= ~(IN_START | IN_ACC);
            } else if (mode == M_OVER && speed == 0 && (pressed & IN_START)) {
                new_game();
            } else if (mode == M_OVER && speed == 0 && (pressed & IN_BTN)) {
                mode = M_TITLE;
                hud_clear();
                set_stage(0);
            }
        }
        {   /* M toggles sound, P pauses */
            static short mute_was, pause_was, paused;
            if ((in & IN_MUTE) && !mute_was) snd_on = !snd_on;
            dbg.sndOn = snd_on;
            mute_was = in & IN_MUTE;
            if ((in & IN_PAUSE) && !pause_was && mode == M_PLAY) {
                paused = !paused;
                if (paused) snd_silence(); else snd_period = -1;
            }
            pause_was = in & IN_PAUSE;
            if (paused) {
                if (mode != M_PLAY) paused = 0;
                else continue;
            }
        }
        tick(in, mdx);
        while (--n > 0) tick(in, 0);
        dbg.prof[5] += TickCount() - now;
        render();
        dbg.frames++;
        fps_f++;
        dbg.ticks = now;
        if (now - fps_t >= 120) {
            dbg.fps10 = (short)((fps_f * 6015L / 10) / (long)(now - fps_t));
            fps_t = now;
            fps_f = 0;
        }
        dbg.mode = mode; dbg.day = day; dbg.quota = quota; dbg.stage = stage;
        dbg.speed = speed; dbg.px = px; dbg.odo10 = odo10; dbg.camz = rs.camz;
        dbg.stageTick = stage_tick; dbg.offroad = offroad; dbg.dayTick = day_tick;
    }
    return 0;
}

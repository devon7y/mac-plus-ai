/*
 * Tune: the Mac composes and plays folk tunes. A small GRU (tune/ref, engine.c) writes a tune one
 * token at a time; each note appears on the staff as it is written, and the player (player.c) plays
 * it with a simple bass, keeping its tempo by the clock. Compose (with the seed shown), Again (a new
 * seed), Play/Stop, Save (ABC text for TeachText) and Copy (the staff as a PICT).
 * Keys: Return = Again, Space = Play/Stop, M = sound on/off, ` (Esc) = stop.
 */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <OSUtils.h>
#include <MacMemory.h>
#include <Resources.h>
#include <Multiverse.h>
#include <Files.h>
#include <StandardFile.h>
#include <ToolUtils.h>
#include <Devices.h>
#include <string.h>
#include "engine.h"
#include "staff.h"
#include "player.h"
#include "abc.h"

#define APP_W 448
#define TICKS (*(volatile unsigned long *)0x16A)

/* ---- debug block / test mailbox (the Lua tests in tune/app/tests find it by its tag) ---------- */
struct Dbg {
    unsigned long magic, version, notMagic;
    long state;                 /* 0 idle, 1 composing, 2 composed, -n errors */
    long cmd, arg;              /* test mailbox: see do_test_cmd */
    unsigned long seed;
    short type, key, mode, meter;
    long ntok;
    unsigned long t_start, t_end;   /* Ticks: composing started / finished */
    unsigned long pdbg[6];
    short nev, nslot, sp, playing;
    long freeMem;
    long cmdDone;
    short cur_note, auto_play;
    unsigned long play_start;
    unsigned char toks[640];
    unsigned long t_engine, t_handle;   /* Ticks spent in the model / in handle_token (drawing, lists) */
};
struct Dbg dbg;
#define DBG_MAGIC 0x544E4442UL      /* 'TNDB' */

/* ---- tables --------------------------------------------------------------------------------- */
typedef struct TypeDef {
    const char *name;           /* menu / display */
    const char *abc;            /* R: field */
    short model;                /* index in vocab.TYPES */
    short meters[3], nmeters;   /* indices in vocab.METERS */
    short bpm, beat;            /* moderate tempo: beats per minute, beat in ticks */
    short bpm2, beat2;          /* for the second meter (6/8 march, 6/8 air) */
} TypeDef;
/* vocab.TYPES: reel jig hornpipe air march waltz slipjig strathspey polka
   vocab.METERS: 4/4 6/8 3/4 2/4 9/8 12/8 3/8 */
static const TypeDef types[] = {
    {"Reel", "Reel", 0, {0}, 1, 168, 12, 0, 0},
    {"Jig", "Jig", 1, {1}, 1, 104, 18, 0, 0},
    {"Hornpipe", "Hornpipe", 2, {0}, 1, 126, 12, 0, 0},
    {"Waltz", "Waltz", 5, {2}, 1, 150, 12, 0, 0},
    {"March", "March", 4, {3, 1}, 2, 112, 12, 100, 18},
    {"Air", "Air", 3, {2, 0, 1}, 3, 76, 12, 52, 18},
    {"Slip Jig", "Slip jig", 6, {4}, 1, 104, 18, 0, 0},
    {"Strathspey", "Strathspey", 7, {0}, 1, 116, 12, 0, 0},
    {"Polka", "Polka", 8, {3}, 1, 136, 12, 0, 0},
};
#define NTYPES 9

typedef struct KeyDef { const char *name, *abc; short li, acc, mode; } KeyDef;
/* mode: 0 major, 1 minor, 2 dorian, 3 mixolydian (vocab.MODES) */
static const KeyDef keys[] = {
    {"Any key", "", 0, 0, -1},
    {"D major", "D", 1, 0, 0}, {"G major", "G", 4, 0, 0}, {"A major", "A", 5, 0, 0},
    {"C major", "C", 0, 0, 0}, {"F major", "F", 3, 0, 0}, {"Bb major", "Bb", 6, -1, 0},
    {"E minor", "Em", 2, 0, 1}, {"A minor", "Am", 5, 0, 1}, {"B minor", "Bm", 6, 0, 1},
    {"D minor", "Dm", 1, 0, 1}, {"G minor", "Gm", 4, 0, 1},
    {"A dorian", "Ador", 5, 0, 2}, {"E dorian", "Edor", 2, 0, 2}, {"D dorian", "Ddor", 1, 0, 2},
    {"G dorian", "Gdor", 4, 0, 2},
    {"D mixolydian", "Dmix", 1, 0, 3}, {"A mixolydian", "Amix", 5, 0, 3}, {"G mixolydian", "Gmix", 4, 0, 3},
};
#define NKEYS 19
static const short mode_first[4] = {1, 7, 12, 16}, mode_count[4] = {6, 5, 4, 3};
static const char *const mode_names[4] = {"major", "minor", "dorian", "mixolydian"};
static const short tempo_pct[4] = {80, 100, 115, 130};

/* ---- state ---------------------------------------------------------------------------------- */
static WindowPtr gWin;
static short gOX;
static MenuHandle mApple, mFile, mEdit, mTune, mKey, mPlay;
enum { M_APPLE = 128, M_FILE, M_EDIT, M_TUNE, M_KEY, M_PLAY };
enum { F_SAVE = 1, F_SEP, F_QUIT };
enum { E_UNDO = 1, E_SEP, E_CUT, E_COPY, E_PASTE, E_CLEAR };
enum { T_COMPOSE = 1, T_AGAIN, T_SEED, T_SEP, T_TYPE0 };
enum { P_PLAY = 1, P_SEP1, P_TEMPO0, P_SEP2 = P_TEMPO0 + 4, P_FIDDLE, P_FLUTE, P_SEP3, P_BASS, P_SOUND };

static short sel_type = 0, sel_key = 0, sel_tempo = 1, sel_timbre = 0, sel_bass = 1, sel_sound = 1;
static unsigned long gSeed;

static TuneModel gModel;
static TuneGen gGen;
static Staff gStaff;
static short gModelOK, gSoundOK;

/* the tune being composed / composed */
static short cur_type, cur_key, cur_mode, cur_meter, cur_tempo_bpm, cur_beat;
static short composing, composed;
static unsigned char toks[640];
static short ntok;
static short pend_alt;
static short frag_start;             /* bar position where the current bass fragment starts */
static short frag_n;
static struct { unsigned char pos, dur; signed char deg; } frag[64];
static short sec_m, sec_b, e1_m, e1_b;
static signed char bar_acc[7][12];
static short nbars_done;
static unsigned long compose_t0;
static short auto_play_done;
static short hl_last = -1;

/* key info of the current tune */
static short k_li, k_acc, k_pc, k_midi, k_oct, k_nsig;
static signed char k_sig[7];

static Rect rButtons[5];
static const char *const btn_names[5] = {"Compose", "Again", "Play", "Save", "Copy"};
static Rect rInfo, rStaff;

/* ---- small helpers -------------------------------------------------------------------------- */
static void c2p(const char *c, Str255 p)
{
    short n = 0;
    while (c[n] && n < 255) {
        p[n + 1] = (unsigned char)c[n];
        n++;
    }
    p[0] = (unsigned char)n;
}

static void pcat(Str255 p, const char *c)
{
    while (*c && p[0] < 255)
        p[++p[0]] = (unsigned char)*c++;
}

static void pcatn(Str255 p, long v)
{
    Str255 t;
    short i;
    NumToString(v, t);
    for (i = 1; i <= t[0] && p[0] < 255; i++)
        p[++p[0]] = t[i];
}

static unsigned long new_seed(void)
{
    unsigned long s = TICKS * 2654435761UL + (unsigned long)Random() * 40503UL;
    return (s ^ (s >> 13)) % 100000UL;
}

/* ---- key -------------------------------------------------------------------------------------- */
static const short letter_pc[7] = {0, 2, 4, 5, 7, 9, 11};
static const signed char maj_sig_by_pc[12] = {0, -5, 2, -3, 4, -1, 6, 1, -4, 3, -2, 5};
static const short mode_shift[4] = {0, 9, 2, 7};

static void set_key(short li, short acc, short mode)
{
    short i, m, n;
    k_li = li;
    k_acc = acc;
    k_pc = (short)((letter_pc[li] + acc + 12) % 12);
    n = maj_sig_by_pc[(k_pc - mode_shift[mode] + 24) % 12];
    k_nsig = n;
    for (i = 0; i < 7; i++)
        k_sig[i] = 0;
    for (i = 0; i < (n > 0 ? n : -n); i++) {
        static const char sh[] = "FCGDAEB", fl[] = "BEADGCF";
        const char *o = n > 0 ? sh : fl;
        short l = (short)(strchr("CDEFGAB", o[i]) - "CDEFGAB");
        k_sig[l] = (signed char)(n > 0 ? 1 : -1);
    }
    for (m = 55; m <= 66; m++)
        if (m % 12 == k_pc)
            break;
    k_midi = m;
    k_oct = (short)((m - letter_pc[li] - acc) / 12 - 1);
}

/* ---- drawing -------------------------------------------------------------------------------- */
static void draw_button(short i, short hilite)
{
    Rect r = rButtons[i];
    Str255 s;
    short w;
    const char *name = btn_names[i];
    if (i == 2 && pl_playing())
        name = "Stop";
    EraseRoundRect(&r, 10, 10);
    FrameRoundRect(&r, 10, 10);
    TextFont(0);
    TextSize(12);
    c2p(name, s);
    w = StringWidth(s);
    MoveTo((short)((r.left + r.right - w) / 2), (short)(r.bottom - 5));
    DrawString(s);
    if (hilite)
        InvertRoundRect(&r, 10, 10);
}

static void draw_info(void)
{
    Str255 s, r;
    const TypeDef *t = &types[composed || composing ? cur_type : sel_type];
    short room;
    s[0] = 0;
    pcat(s, t->name);
    pcat(s, " in ");
    if (composed || composing) {
        char c[3] = {"CDEFGAB"[k_li], k_acc < 0 ? 'b' : 0, 0};
        pcat(s, c);
        pcat(s, " ");
        pcat(s, mode_names[cur_mode]);
    } else {
        pcat(s, keys[sel_key].name);
    }
    pcat(s, "  \xA5  ");
    pcatn(s, (long)(composed || composing ? cur_tempo_bpm : (long)t->bpm * tempo_pct[sel_tempo] / 100));
    pcat(s, " bpm  \xA5  seed ");
    pcatn(s, (long)gSeed);
    r[0] = 0;
    if (!gModelOK)
        pcat(r, "Model missing!");
    else if (composing) {
        pcat(r, "composing bar ");
        pcatn(r, nbars_done + 1);
    } else if (composed) {
        pcatn(r, nbars_done);
        pcat(r, " bars in ");
        pcatn(r, (long)((dbg.t_end - dbg.t_start) / 60));
        pcat(r, " s");
    } else
        pcat(r, "press Compose");
    if (pl_playing())
        pcat(r, "  \xA5  playing");
    else if (!sel_sound)
        pcat(r, "  \xA5  sound off");
    EraseRect(&rInfo);
    TextFont(3);
    TextSize(9);
    room = (short)(rInfo.right - rInfo.left - 4 - StringWidth(r) - 8);
    while (s[0] > 0 && StringWidth(s) > room)
        s[0]--;
    MoveTo(rInfo.left + 2, rInfo.top + 10);
    DrawString(s);
    MoveTo((short)(rInfo.right - StringWidth(r) - 2), rInfo.top + 10);
    DrawString(r);
}

static void draw_all(void)
{
    short i;
    SetPort(gWin);
    EraseRect(&gWin->portRect);
    for (i = 0; i < 5; i++)
        draw_button(i, 0);
    draw_info();
    if (composed || composing)
        st_draw_all(&gStaff);
    else {
        Str255 s;
        TextFont(3);
        TextSize(9);
        c2p("Choose a tune type and key from the menus, then click Compose.", s);
        MoveTo((short)((rStaff.left + rStaff.right - StringWidth(s)) / 2), rStaff.top + 100);
        DrawString(s);
    }
    hl_last = -1;
}

/* ---- composing ------------------------------------------------------------------------------- */
static void bar_acc_reset(void)
{
    short a, b;
    for (a = 0; a < 7; a++)
        for (b = 0; b < 12; b++)
            bar_acc[a][b] = 99;
}

static const signed char chord_cands[4][6] = {{0, 4, 3, 5, 1, -1}, {0, 4, 3, 6, 2, 5},
                                              {0, 3, 6, 2, 4, -1}, {0, 3, 6, 4, 1, -1}};

static short deg_midi_bass(short deg)
{
    short q = deg >= 0 ? deg / 7 : -((-deg + 6) / 7), r = (short)(deg - 7 * q);
    short m = (short)(k_midi - 12 + 12 * q + tk_mode_steps[cur_mode][r]);
    while (m > 52)
        m -= 12;
    while (m < 38)
        m += 12;
    return m;
}

static short choose_chord(short h0, short h1)
{
    short best = 0, bs = -1, c, j;
    for (c = 0; c < 6; c++) {
        short r = chord_cands[cur_mode][c], sc = 0;
        if (r < 0)
            break;
        for (j = 0; j < frag_n; j++) {
            short p = frag[j].pos, e = (short)(p + frag[j].dur), o0 = p > h0 ? p : h0, o1 = e < h1 ? e : h1;
            short d = (short)(((frag[j].deg - r) % 7 + 7) % 7);
            if (o1 <= o0)
                continue;
            if (d == 0 || d == 2 || d == 4) {
                short w = (short)(o1 - o0);
                if (p == h0)
                    w *= 3;
                else if ((p - h0) % (cur_beat) == 0)
                    w *= 2;
                sc += d == 0 ? w + w / 2 : w;
            }
        }
        if (c == 0)
            sc += (h1 - h0) / 2;
        if (sc > bs) {
            bs = sc;
            best = r;
        }
    }
    return best;
}

/* bass notes for the bar positions [p0, p1) of the current fragment */
static void bass_fragment(short p0, short p1)
{
    short blen = gStaff.blen, bt = gStaff.meter_den == 8 ? 18 : 12;
    short hs = gStaff.meter_den == 8 ? blen : (blen > 24 ? 24 : blen);
    short q = p0;
    while (q < p1) {
        short b0 = (short)((q / bt) * bt), b1 = (short)(b0 + bt);
        short h0, h1, r, root, fifth;
        if (b1 > p1)
            b1 = p1;
        if (q != b0) {                       /* partial beat (a pickup): no bass */
            pl_add_bass((short)(b1 - q), 0);
            q = b1;
            continue;
        }
        h0 = (short)((q / hs) * hs);
        h1 = (short)(h0 + hs > blen ? blen : h0 + hs);
        r = choose_chord(h0, h1);
        root = deg_midi_bass(r);
        fifth = deg_midi_bass((short)(r + 4));
        if (fifth > root)
            fifth -= 12;
        if (fifth < 33)
            fifth += 12;
        pl_add_bass((short)(b1 - q), (unsigned char)(q == h0 ? root : fifth));
        q = b1;
    }
}

static void close_fragment(short p1)
{
    if (p1 > frag_start)
        bass_fragment(frag_start, p1);
    frag_n = 0;
}

static void start_compose(short again)
{
    const TypeDef *t;
    short mode, li, acc, met;
    if (!gModelOK)
        return;
    if (again)
        gSeed = new_seed();
    pl_stop();
    pl_reset();
    cur_type = sel_type;
    t = &types[cur_type];
    met = t->meters[(gSeed >> 3) % t->nmeters];
    cur_meter = met;
    if (keys[sel_key].mode < 0) {             /* any key: mode and key from the seed */
        short r = (short)((gSeed >> 5) % 100);
        mode = r < 66 ? 0 : r < 80 ? 1 : r < 91 ? 2 : 3;
        cur_key = (short)(mode_first[mode] + (gSeed >> 9) % mode_count[mode]);
    } else {
        cur_key = sel_key;
        mode = keys[sel_key].mode;
    }
    cur_mode = mode;
    li = keys[cur_key].li;
    acc = keys[cur_key].acc;
    set_key(li, acc, mode);
    if ((met == 1 || met == 4) && t->nmeters > 1) {
        cur_tempo_bpm = (short)((long)t->bpm2 * tempo_pct[sel_tempo] / 100);
        cur_beat = t->beat2;
    } else {
        cur_tempo_bpm = (short)((long)t->bpm * tempo_pct[sel_tempo] / 100);
        cur_beat = t->beat;
    }
    if (met == 1 || met == 4 || met == 5)
        cur_beat = 18;
    pl_set_tempo(cur_tempo_bpm, cur_beat);
    st_reset(&gStaff, &rStaff, tk_meters[met][0], tk_meters[met][1], k_li, k_oct, k_midi, mode, k_nsig, k_sig);
    tg_start(&gGen, t->model, met, mode, gSeed);
    ntok = 0;
    toks[ntok++] = TK_BOS;
    dbg.toks[0] = TK_BOS;
    pend_alt = 0;
    frag_start = 0;
    frag_n = 0;
    sec_m = sec_b = 0;
    e1_m = e1_b = 0;
    nbars_done = 0;
    bar_acc_reset();
    composing = 1;
    composed = 0;
    auto_play_done = 0;
    compose_t0 = TICKS;
    dbg.t_start = TICKS;
    dbg.seed = gSeed;
    dbg.type = cur_type;
    dbg.key = cur_key;
    dbg.mode = mode;
    dbg.meter = met;
    dbg.state = 1;
    dbg.ntok = 1;
    draw_all();
}

static void finish_compose(void)
{
    composing = 0;
    composed = 1;
    dbg.t_end = TICKS;
    dbg.state = 2;
    pl_finished();
    if (sel_sound && !pl_playing() && !auto_play_done) {
        pl_start();
        auto_play_done = 1;
        dbg.play_start = TICKS;
    }
    draw_button(2, 0);
    draw_info();
}

static void handle_token(short t, short pos0, short trip0)
{
    Ev e;
    short blen = gStaff.blen;
    memset(&e, 0, sizeof e);
    if (ntok < (short)sizeof toks)
        toks[ntok++] = (unsigned char)t;
    dbg.ntok = ntok;
    if (ntok <= (short)sizeof dbg.toks)
        dbg.toks[ntok - 1] = (unsigned char)t;
    if (t >= TK_PICK0 && t < TK_REST0) {
        gStaff.pickup = tk_picks[t - TK_PICK0];
        frag_start = (short)((blen - gStaff.pickup) % blen);
        return;
    }
    if (t == TK_UP || t == TK_DOWN) {
        pend_alt = t == TK_UP ? 1 : -1;
        return;
    }
    if (t == TK_T3)
        return;
    if (t >= TK_REST0) {
        short isnote = t >= TK_NOTE0, d, real;
        d = isnote ? tk_durs[(t - TK_NOTE0) % TK_NDURS] : tk_durs[t - TK_REST0];
        real = trip0 ? (short)(d * 2 / 3) : d;
        e.kind = isnote ? EV_NOTE : EV_REST;
        e.dur = (unsigned char)d;
        e.real = (unsigned char)real;
        e.pos = (unsigned char)pos0;
        e.trip = (unsigned char)(trip0 ? 4 - trip0 : 0);
        if (isnote) {
            short deg = (short)((t - TK_NOTE0) / TK_NDURS + TK_DEG_MIN), l, o, midi, nat, ac, cur, oi;
            e.deg = (signed char)deg;
            e.alt = (signed char)pend_alt;
            midi = st_note_midi(&gStaff, deg, pend_alt);
            e.midi = (unsigned char)midi;
            st_letter(&gStaff, deg, &l, &o);
            nat = (short)(12 * (o + 1) + letter_pc[l]);
            ac = (short)(midi - nat);
            oi = (short)(o + 2 < 0 ? 0 : o + 2 > 11 ? 11 : o + 2);
            cur = bar_acc[l][oi] != 99 ? bar_acc[l][oi] : k_sig[l];
            if (ac != cur) {
                e.acc = (unsigned char)(ac == 1 ? 1 : ac == -1 ? 2 : 3);
                bar_acc[l][oi] = (signed char)ac;
            }
            if (frag_n < 64) {
                frag[frag_n].pos = (unsigned char)pos0;
                frag[frag_n].dur = (unsigned char)real;
                frag[frag_n].deg = (signed char)deg;
                frag_n++;
            }
        }
        pend_alt = 0;
        {
            short idx = st_add(&gStaff, &e, 0);
            pl_add_melody(real, isnote ? e.midi : 0, idx);
        }
        return;
    }
    /* bar lines and the end */
    {
        short full = pos0 >= blen, newslot;
        e.kind = EV_BAR;
        e.tok = (unsigned char)t;
        e.pos = (unsigned char)(full ? blen : pos0);
        newslot = t == TK_BAR || t == TK_E1 || t == TK_E2 || (full && t != TK_EOS);
        st_add(&gStaff, &e, newslot);
        bar_acc_reset();
        close_fragment(full ? blen : pos0);
        if (full || t == TK_BAR || t == TK_E1)
            nbars_done++;
        switch (t) {
        case TK_RS:
        case TK_DBL:
            sec_m = pl_nmel();
            sec_b = pl_nbas();
            break;
        case TK_RE:
        case TK_RER:
            {
                short m1 = pl_nmel(), b1 = pl_nbas();
                pl_copy(sec_m, m1, sec_b, b1);
                sec_m = pl_nmel();
                sec_b = pl_nbas();
            }
            break;
        case TK_E1:
            e1_m = pl_nmel();
            e1_b = pl_nbas();
            break;
        case TK_E2:
            pl_copy(sec_m, e1_m, sec_b, e1_b);
            break;
        }
        frag_start = t == TK_EOS ? 0 : gGen.g.pos;
        if (frag_start >= blen)
            frag_start = 0;
        if (t == TK_EOS)
            finish_compose();
        else
            pl_mark_ready();
    }
}

static void compose_step(void)
{
    short t, pos0 = gGen.g.pos, trip0 = gGen.g.trip;
    unsigned long t0 = TICKS, t1;
    t = tg_sample(&gGen);
    if (t != TK_EOS)
        tg_feed(&gGen, t);
    t1 = TICKS;
    dbg.t_engine += t1 - t0;
    handle_token(t, pos0, trip0);
    dbg.t_handle += TICKS - t1;
    if (t != TK_EOS && (t == TK_BAR || (t >= TK_DBL && t <= TK_E2)))
        draw_info();
    /* start playing early when the composer will stay ahead: the music already written plays for
       longer than the rest takes to write (from the speed so far, +15 % once the sound runs, and
       a long tune of this type: the 90th percentile of the model's token counts) */
    if (composing && sel_sound && !auto_play_done && !pl_playing() && ntok >= 24) {
        static const short est_tok[NTYPES] = {151, 120, 155, 165, 185, 150, 150, 150, 150};
        long el = (long)(TICKS - compose_t0), per = el * 115 / (100L * ntok);
        long rest = est_tok[cur_type] - ntok;
        long left = (rest > 10 ? rest : 10) * per;
        long play = pl_ready_ticks() * 3609L / ((long)cur_tempo_bpm * cur_beat);   /* Mac ticks */
        if (play > left + 120) {
            pl_start();
            auto_play_done = 1;
            dbg.play_start = TICKS;
            draw_button(2, 0);
        }
    }
}

/* ---- commands ------------------------------------------------------------------------------- */
static void toggle_play(void)
{
    if (pl_playing())
        pl_stop();
    else if (composed || composing)
        pl_start();
    draw_button(2, 0);
    draw_info();
}

static void copy_staff(void)
{
    PicHandle pic;
    long len;
    if (!(composed || composing))
        return;
    st_highlight(&gStaff, -1);
    SetPort(gWin);
    ClipRect(&rStaff);
    pic = OpenPicture(&rStaff);
    st_draw_all(&gStaff);
    ClosePicture();
    ClipRect(&gWin->portRect);
    if (!pic)
        return;
    len = GetHandleSize((Handle)pic);
    ZeroScrap();
    HLock((Handle)pic);
    PutScrap(len, 'PICT', (Ptr)*pic);
    HUnlock((Handle)pic);
    KillPicture(pic);
    dbg.cmdDone = 0x434F5059;          /* 'COPY' */
}

static void fill_abcinfo(AbcInfo *k, char *title)
{
    char *p = title;
    const char *tn = types[cur_type].name;
    while (*tn)
        *p++ = *tn++;
    {
        const char *s = " for seed ";
        while (*s)
            *p++ = *s++;
    }
    {
        char b[12];
        short n = 0;
        unsigned long v = gSeed;
        do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n)
            *p++ = b[--n];
    }
    *p = 0;
    k->seed = gSeed;
    k->title = title;
    k->type_name = types[cur_type].abc;
    k->key_name = keys[cur_key].abc;
    k->meter_num = tk_meters[cur_meter][0];
    k->meter_den = tk_meters[cur_meter][1];
    k->mode = cur_mode;
    k->tonic_li = k_li;
    k->tonic_oct = k_oct;
    k->tonic_midi = k_midi;
    memcpy(k->sig, k_sig, 7);
}

static OSErr write_text_file(ConstStr255Param name, short vRef, const char *buf, long len)
{
    OSErr err;
    short ref;
    err = Create(name, vRef, 'ttxt', 'TEXT');
    if (err != noErr && err != dupFNErr)
        return err;
    err = FSOpen(name, vRef, &ref);
    if (err != noErr)
        return err;
    SetEOF(ref, 0);
    err = FSWrite(ref, &len, buf);
    FSClose(ref);
    FlushVol(NULL, vRef);
    return err;
}

static void save_abc(short ask)
{
    static char buf[6000];
    char title[64];
    AbcInfo k;
    long len;
    Str255 name;
    if (!composed && !composing)
        return;
    fill_abcinfo(&k, title);
    len = abc_write(buf, sizeof buf, toks, ntok, &k);
    c2p(title, name);
    if (ask) {
        SFReply reply;
        Point where;
        where.h = (short)(gOX + 80);
        where.v = 80;
        SFPutFile(where, "\pSave the tune (ABC text) as:", name, NULL, &reply);
        if (reply.good)
            write_text_file(reply.fName, reply.vRefNum, buf, len);
        draw_all();
    } else {
        write_text_file("\pTune.abc", 0, buf, len);
        dbg.cmdDone = 0x53415645;      /* 'SAVE' */
    }
}

static void about(void)
{
    WindowPtr w;
    Rect r;
    EventRecord ev;
    static const char *const lines[] = {
        "Tune",
        "The Macintosh composes folk tunes.",
        "",
        "A small neural network (a two-layer GRU with ternary weights,",
        "about 400,000 parameters) writes each tune note by note.",
        "It learned from public-domain dance tunes and airs:",
        "O'Neill's Music of Ireland (1903; ABC by the O'Neill's Project),",
        "Ryan's Mammoth Collection (1883) and Kerr's Merry Melodies (c. 1880;",
        "ABC by John Chambers and contributors). Every tune is new.",
        "",
        "Click or press a key to continue.",
    };
    short i, n = sizeof lines / sizeof lines[0];
    SetRect(&r, gOX + 24, 60, gOX + APP_W - 24, 60 + 24 + n * 13);
    w = NewWindow(NULL, &r, "\p", true, dBoxProc, (WindowPtr)-1, false, 0);
    SetPort(w);
    for (i = 0; i < n; i++) {
        Str255 s;
        TextFont(i == 0 ? 0 : 3);
        TextSize(i == 0 ? 12 : 9);
        c2p(lines[i], s);
        MoveTo(14, 20 + i * 13);
        DrawString(s);
    }
    dbg.cmdDone = 0x41424F55;          /* 'ABOU' */
    for (;;) {
        SystemTask();
        if (GetNextEvent(mDownMask | keyDownMask, &ev))
            break;
        if (dbg.cmd == 99) {           /* test: close */
            dbg.cmd = 0;
            break;
        }
    }
    DisposeWindow(w);
    SetPort(gWin);
}

static void seed_dialog(void)
{
    WindowPtr w;
    Rect r, box;
    EventRecord ev;
    unsigned long v = gSeed;
    short done = 0;
    SetRect(&r, gOX + 120, 100, gOX + APP_W - 120, 170);
    w = NewWindow(NULL, &r, "\p", true, dBoxProc, (WindowPtr)-1, false, 0);
    SetPort(w);
    SetRect(&box, 70, 28, 190, 46);
    while (!done) {
        Str255 s;
        TextFont(0);
        TextSize(12);
        MoveTo(12, 20);
        DrawString("\pSeed (0 to 99999):");
        EraseRect(&box);
        FrameRect(&box);
        NumToString((long)v, s);
        MoveTo(box.left + 6, box.bottom - 4);
        DrawString(s);
        TextFont(3);
        TextSize(9);
        MoveTo(12, 62);
        DrawString("\pType the number; Return composes, Esc cancels.");
        SystemTask();
        if (GetNextEvent(keyDownMask | mDownMask, &ev)) {
            if (ev.what == mouseDown)
                done = 2;
            else {
                char c = (char)(ev.message & charCodeMask);
                if (c >= '0' && c <= '9' && v < 10000)
                    v = v * 10 + (unsigned long)(c - '0');
                else if (c == 8)
                    v /= 10;
                else if (c == 13 || c == 3)
                    done = 1;
                else if (c == 27 || c == '`')
                    done = 2;
            }
        }
    }
    DisposeWindow(w);
    SetPort(gWin);
    if (done == 1) {
        gSeed = v;
        start_compose(0);
    } else
        draw_all();
}

/* ---- menus ---------------------------------------------------------------------------------- */
static void check_menus(void)
{
    short i;
    for (i = 0; i < NTYPES; i++)
        CheckItem(mTune, (short)(T_TYPE0 + i + (i >= 6)), i == sel_type);
    for (i = 0; i < NKEYS; i++)
        CheckItem(mKey, (short)(1 + i + (i >= 1) + (i >= 7) + (i >= 12) + (i >= 16)), i == sel_key);
    for (i = 0; i < 4; i++)
        CheckItem(mPlay, (short)(P_TEMPO0 + i), i == sel_tempo);
    CheckItem(mPlay, P_FIDDLE, sel_timbre == 0);
    CheckItem(mPlay, P_FLUTE, sel_timbre == 1);
    CheckItem(mPlay, P_BASS, sel_bass);
    CheckItem(mPlay, P_SOUND, sel_sound);
}

static void setup_menus(void)
{
    short i;
    Str255 s;
    mApple = NewMenu(M_APPLE, "\p\x14");
    AppendMenu(mApple, "\pAbout Tune\xC9;(-");
    AppendResMenu(mApple, 'DRVR');
    InsertMenu(mApple, 0);
    mFile = NewMenu(M_FILE, "\pFile");
    AppendMenu(mFile, "\pSave as ABC\xC9/S;(-;Quit/Q");
    InsertMenu(mFile, 0);
    mEdit = NewMenu(M_EDIT, "\pEdit");
    AppendMenu(mEdit, "\p(Undo/Z;(-;(Cut/X;Copy/C;(Paste/V;(Clear");
    InsertMenu(mEdit, 0);
    mTune = NewMenu(M_TUNE, "\pTune");
    AppendMenu(mTune, "\pCompose/K;Again/G;Seed\xC9/E;(-");
    for (i = 0; i < NTYPES; i++) {
        if (i == 6)
            AppendMenu(mTune, "\p(-");
        AppendMenu(mTune, "\px");
        c2p(types[i].name, s);
        SetMenuItemText(mTune, CountMItems(mTune), s);
    }
    InsertMenu(mTune, 0);
    mKey = NewMenu(M_KEY, "\pKey");
    for (i = 0; i < NKEYS; i++) {
        if (i == 1 || i == 7 || i == 12 || i == 16)
            AppendMenu(mKey, "\p(-");
        AppendMenu(mKey, "\px");
        c2p(keys[i].name, s);
        SetMenuItemText(mKey, CountMItems(mKey), s);
    }
    InsertMenu(mKey, 0);
    mPlay = NewMenu(M_PLAY, "\pPlay");
    AppendMenu(mPlay, "\pPlay or Stop/P;(-;Slow;Moderate;Lively;Fast;(-;Fiddle;Flute;(-;Bass;Sound/M");
    InsertMenu(mPlay, 0);
    check_menus();
    DrawMenuBar();
}

static short item_to_index(short item, const short *seps, short nseps)
{
    short i, k = item;
    for (i = 0; i < nseps; i++)
        if (item > seps[i])
            k--;
    return k;
}

static void do_menu(long mr)
{
    short menu = (short)(mr >> 16), item = (short)mr;
    switch (menu) {
    case M_APPLE:
        if (item == 1)
            about();
        else {
            Str255 name;
            GrafPtr old;
            GetPort(&old);
            GetMenuItemText(mApple, item, name);
            OpenDeskAcc(name);
            SetPort(old);
        }
        break;
    case M_FILE:
        if (item == F_SAVE)
            save_abc(1);
        else if (item == F_QUIT) {
            pl_quit();
            ExitToShell();
        }
        break;
    case M_EDIT:
        if (!SystemEdit((short)(item - 1)) && item == E_COPY)
            copy_staff();
        break;
    case M_TUNE:
        if (item == T_COMPOSE)
            start_compose(0);
        else if (item == T_AGAIN)
            start_compose(1);
        else if (item == T_SEED)
            seed_dialog();
        else if (item >= T_TYPE0) {
            static const short seps[1] = {T_TYPE0 + 6};
            sel_type = (short)(item_to_index(item, seps, 1) - T_TYPE0);
            check_menus();
            draw_info();
        }
        break;
    case M_KEY:
        {
            static const short seps[4] = {2, 9, 15, 20};
            sel_key = (short)(item_to_index(item, seps, 4) - 1);
            check_menus();
            draw_info();
        }
        break;
    case M_PLAY:
        if (item == P_PLAY)
            toggle_play();
        else if (item >= P_TEMPO0 && item < P_TEMPO0 + 4) {
            sel_tempo = (short)(item - P_TEMPO0);
            if (composed || composing) {
                const TypeDef *t = &types[cur_type];
                short base = (cur_beat == 18 && t->nmeters > 1) ? t->bpm2 : t->bpm;
                cur_tempo_bpm = (short)((long)base * tempo_pct[sel_tempo] / 100);
                pl_set_tempo(cur_tempo_bpm, cur_beat);
            }
            draw_info();
        } else if (item == P_FIDDLE || item == P_FLUTE) {
            sel_timbre = (short)(item == P_FLUTE);
            pl_set_timbre(sel_timbre);
        } else if (item == P_BASS) {
            sel_bass = !sel_bass;
            pl_set_bass(sel_bass);
        } else if (item == P_SOUND) {
            sel_sound = !sel_sound;
            pl_set_sound(sel_sound);
            draw_info();
        }
        check_menus();
        break;
    }
    HiliteMenu(0);
}

/* ---- events --------------------------------------------------------------------------------- */
static void click_buttons(Point p)
{
    short i;
    for (i = 0; i < 5; i++) {
        if (PtInRect(p, &rButtons[i])) {
            short in = 1;
            draw_button(i, 1);
            while (StillDown()) {
                Point q;
                GetMouse(&q);
                if (PtInRect(q, &rButtons[i]) != in) {
                    in = !in;
                    draw_button(i, in);
                }
            }
            draw_button(i, 0);
            if (!in)
                return;
            if (i == 0) start_compose(0);
            else if (i == 1) start_compose(1);
            else if (i == 2) toggle_play();
            else if (i == 3) save_abc(1);
            else copy_staff();
            return;
        }
    }
}

static void do_key(char c, short cmd)
{
    if (cmd) {
        do_menu(MenuKey(c));
        return;
    }
    if (c == 13 || c == 3)
        start_compose(1);
    else if (c == ' ')
        toggle_play();
    else if (c == 'm' || c == 'M')
        do_menu(((long)M_PLAY << 16) | P_SOUND);
    else if (c == '`' || c == 27) {
        pl_stop();
        draw_button(2, 0);
        draw_info();
    }
}

static void do_test_cmd(void)
{
    long c = dbg.cmd, a = dbg.arg;
    if (!c)
        return;
    dbg.cmd = 0;
    switch (c) {
    case 1: gSeed = (unsigned long)a; start_compose(0); break;      /* compose with seed */
    case 2: if (!pl_playing()) toggle_play(); break;
    case 3: if (pl_playing()) toggle_play(); break;
    case 4: copy_staff(); break;
    case 5: sel_type = (short)a; check_menus(); draw_info(); break;
    case 6: sel_key = (short)a; check_menus(); draw_info(); break;
    case 7: save_abc(0); break;
    case 8: about(); break;
    case 9: sel_tempo = (short)a; check_menus(); break;
    }
}

int main(void)
{
    Rect b;
    Handle h;
    short i, bw;
    dbg.magic = DBG_MAGIC;
    dbg.version = 1;
    dbg.notMagic = ~DBG_MAGIC;
    dbg.state = -100;                  /* starting up */
    MaxApplZone();
    MoreMasters();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);
    qd.randSeed = (long)TICKS;
    b = qd.screenBits.bounds;
    gOX = (short)((b.left + b.right - APP_W) / 2);
    gWin = NewWindow(NULL, &b, "\p", true, plainDBox, (WindowPtr)-1, false, 0);
    SetPort(gWin);
    bw = 80;
    for (i = 0; i < 5; i++)
        SetRect(&rButtons[i], gOX + 8 + i * (bw + 7), 25, gOX + 8 + i * (bw + 7) + bw, 43);
    SetRect(&rInfo, gOX + 4, 46, gOX + APP_W - 4, 58);
    SetRect(&rStaff, gOX, 60, gOX + APP_W, b.bottom - 2);
    setup_menus();
    h = GetResource('TMDL', 128);
    if (h) {
        HLock(h);
        if (tm_load(&gModel, *h, GetHandleSize(h)) == 0) {
            void *work = NewPtr(tg_bytes(&gModel));
            if (work) {
                tg_init(&gGen, &gModel, work);
                gGen.maxbars = 32;
                gModelOK = 1;
            }
        }
    }
    gSoundOK = pl_init() == 0;
    pl_set_bass(sel_bass);
    dbg.state = gModelOK ? 0 : -1;
    dbg.freeMem = FreeMem();
    gSeed = new_seed();
    draw_all();
    for (;;) {
        EventRecord ev;
        if (composing)
            compose_step();
        SystemTask();
        if (GetNextEvent(everyEvent, &ev)) {
            switch (ev.what) {
            case mouseDown:
                {
                    WindowPtr w;
                    short part = FindWindow(ev.where, &w);
                    if (part == inMenuBar)
                        do_menu(MenuSelect(ev.where));
                    else if (part == inSysWindow)
                        SystemClick(&ev, w);
                    else if (w == gWin) {
                        Point p = ev.where;
                        GlobalToLocal(&p);
                        click_buttons(p);
                    }
                }
                break;
            case keyDown:
            case autoKey:
                do_key((char)(ev.message & charCodeMask), (ev.modifiers & cmdKey) != 0);
                break;
            case updateEvt:
                if ((WindowPtr)ev.message == gWin) {
                    BeginUpdate(gWin);
                    draw_all();
                    EndUpdate(gWin);
                }
                break;
            }
        }
        do_test_cmd();
        {
            short cn = pl_cur_note();
            if (cn != hl_last) {
                SetPort(gWin);
                st_highlight(&gStaff, cn);
                hl_last = cn;
            }
        }
        {
            static short was_playing;
            short p = pl_playing();
            if (p != was_playing) {
                was_playing = p;
                draw_button(2, 0);
                draw_info();
            }
        }
        pl_debug(dbg.pdbg);
        dbg.nev = gStaff.nev;
        dbg.nslot = gStaff.nslot;
        dbg.sp = gStaff.sp;
        dbg.playing = pl_playing();
        dbg.cur_note = pl_cur_note();
    }
    return 0;
}

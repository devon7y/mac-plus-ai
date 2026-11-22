/*
 * Neural Pinball for the Macintosh Plus: Toolbox setup, menus, input, the attract/play/game-over
 * flow, timing and the debug block.
 *
 * Timing: the game advances one tick per Mac tick (60.15 Hz), each tick being STEPS_PER_TICK
 * physics steps (481 Hz). The keyboard map and mouse button are read every 2 physics steps
 * (about 4 ms), so flipper timing is finer than the frame rate. After the ticks that are due,
 * the frame is drawn (only what changed); if drawing falls behind, several ticks run before the
 * next frame, so the physics always keeps real time (up to 4 ticks of catch-up).
 *
 * Build flags: -DHASHTEST: before anything else, play 3 minutes of the seeded AI game without
 * drawing and store the physics hash in the debug block (must equal tests/gamesim --hash).
 * -DBENCH: measure physics and drawing costs into the debug block.
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
#include <ToolUtils.h>
#include <Devices.h>
#include "game.h"
#include "render.h"
#include "pbsound.h"
#include "hiscore.h"
#include "speech.h"

#define TICKS (*(volatile unsigned long *)ticks_addr)
static volatile long ticks_addr = 0x16A;
#define KEYMAP ((volatile unsigned char *)keymap_addr)
static volatile long keymap_addr = 0x174;
#define MBSTATE (*(volatile unsigned char *)mbstate_addr)
static volatile long mbstate_addr = 0x172;

#define MAGIC 0x4E50424CUL     /* 'NPBL' */

/* ------------------------------------------------------------------ debug block (Lua reads it) */
struct Dbg {
    unsigned long magic, version, notmagic;  /*  0 */
    long mode;                               /* 12: 0 init 1 attract 2 play 3 over 4 paused 5 initials */
    long frames;                             /* 16: frames drawn */
    long ticks;                              /* 20: game ticks run */
    long score;                              /* 24 */
    short ball, gstate, ai, inplay;          /* 28..34 */
    long fps100;                             /* 36: frames per second x 100 (last window) */
    long tps100;                             /* 40: game ticks per second x 100 (last window) */
    short bx[MAX_BALLS], by[MAX_BALLS];      /* 44: ball positions (px) */
    short flipf[2];                          /* 60 */
    long freemem;                            /* 64 */
    unsigned long hash;                      /* 68: HASHTEST result */
    long late;                               /* 72: ticks dropped (more than 4 behind) */
    long bench_phys, bench_draw;             /* 76, 80: BENCH: ticks for 600 game ticks / 600 frames */
    unsigned short buttons;                  /* 84 */
    short face;                              /* 86 */
    long copied;                             /* 88 */
    long screen_x;                           /* 92: comp_x */
    long hash_ticks;                         /* 96 */
    long cmd;                                /* 100: Lua -> app: 1 start a game */
    long anchor;                             /* 104: runtime address of prof_anchor (profiling) */
    long bench[8];                           /* 108: HASHTEST micro-benchmarks, ticks per 2000 steps (8) */
    char top_init[4];                        /* 140: initials of the best high score */
    long top_score;                          /* 144 */
    short speech, spoken;                    /* 148: MacinTalk found, callouts spoken */
} dbg;

static Game G;
static WindowPtr win;
static MenuHandle mApple, mFile;
enum { M_APPLE = 128, M_FILE = 129 };
enum { I_NEW = 1, I_PAUSE = 2, I_SOUND = 4, I_SPEECH = 5, I_SCORES = 7, I_QUIT = 9 };

static short paused, quitting, show_scores_t, spoke;
static short initials_mode, initials_rank, ninit;
static char initials[4];
static unsigned char km_prev[16], km_now[16];
static unsigned long over_since;

/* ------------------------------------------------------------------ input */

static short key(short c)
{
    return (KEYMAP[c >> 3] >> (c & 7)) & 1;
}

static void snap_keys(void)
{
    short i;
    for (i = 0; i < 16; i++) {
        km_prev[i] = km_now[i];
        km_now[i] = KEYMAP[i];
    }
}

static short pressed(short c)
{
    return ((km_now[c >> 3] >> (c & 7)) & 1) && !((km_prev[c >> 3] >> (c & 7)) & 1);
}

#define K_A 0x00
#define K_S 0x01
#define K_D 0x02
#define K_Z 0x06
#define K_W 0x0D
#define K_P 0x23
#define K_RETURN 0x24
#define K_ENTER 0x4C
#define K_M 0x2E
#define K_SLASH 0x2C
#define K_SPACE 0x31
#define K_SHIFT 0x38
#define K_ESC 0x35
#define K_GRAVE 0x32        /* the Plus keyboard has no Esc: emu/play.sh maps the host's Esc here */

static short ball_on_plunger(void)
{
    short i;
    for (i = 0; i < MAX_BALLS; i++) {
        const Ball *b = &G.w.ball[i];
        if (b->state == BALL_FREE && b->x > ((fx)PLUNGER_X0 << 10) && b->y > ((fx)(PLUNGER_Y - 2560) << 10))
            return 1;
    }
    return 0;
}

/* Standard keys (DECISIONS.md): A / D flippers (also Z and /, the arrow keys, Shift = both,
   the mouse button = both); Space or S pulls the plunger when a ball sits on it, otherwise
   Space (or W) nudges. The Plus keyboard reports only two ordinary keys at once; Shift and the
   mouse button don't count, so "both flippers + nudge" works with Shift + W. */
static unsigned short read_buttons(void)
{
    unsigned short b = 0;
    short shift = key(K_SHIFT), mouse = !(MBSTATE & 0x80);
    if (key(K_A) || key(K_Z) || key(0x46) || key(0x7B) || shift || mouse)
        b |= BTN_LEFT;
    if (key(K_D) || key(K_SLASH) || key(0x42) || key(0x7C) || shift || mouse)
        b |= BTN_RIGHT;
    if (ball_on_plunger()) {
        if (key(K_SPACE) || key(K_S) || key(0x48) || key(0x7D))
            b |= BTN_PLUNGER;
    } else if (key(K_SPACE))
        b |= BTN_NUDGE;
    if (key(K_W) || key(0x4D) || key(0x7E))
        b |= BTN_NUDGE;
    return b;
}

/* ------------------------------------------------------------------ menus, About */

static void setup_menus(void)
{
    mApple = NewMenu(M_APPLE, "\p\x14");
    AppendMenu(mApple, "\pAbout Neural Pinball\xC9;(-");
    AppendResMenu(mApple, 'DRVR');
    InsertMenu(mApple, 0);
    mFile = NewMenu(M_FILE, "\pFile");
    AppendMenu(mFile, "\pNew Game/N;Pause/P;(-;Sound;Speech;(-;High Scores;(-;Quit/Q");
    InsertMenu(mFile, 0);
    DrawMenuBar();
}

static void update_menus(void)
{
    CheckItem(mFile, I_PAUSE, paused);
    CheckItem(mFile, I_SOUND, sound_is_on());
    CheckItem(mFile, I_SPEECH, speech_available && speech_on);
    if (speech_available)
        EnableItem(mFile, I_SPEECH);
    else
        DisableItem(mFile, I_SPEECH);
}

static void about(void)
{
    WindowPtr w;
    Rect r;
    EventRecord ev;
    static const unsigned char *const lines[] = {
        "\pNeural Pinball",
        "\pAn AI-themed pinball table for the Macintosh Plus.",
        "\p",
        "\pA real ball: 16.16 fixed-point physics at 481 steps",
        "\pa second, flippers that move, cradles and live catches.",
        "\pWhen nobody plays, the Mac plays. Press Return to",
        "\pstart a game, or a flipper key to take over the ball.",
        "\p",
        "\pFlippers: A and D (or Z and /, the arrows, Shift,",
        "\pthe mouse button). Plunger: hold Space or S.",
        "\pNudge: W (Space while the ball is in play).",
        "\pP pause, M sound, Esc quits.",
        "\p",
        "\pOriginal code and art. Click to continue.",
    };
    short i, n = sizeof lines / sizeof lines[0];
    ShowCursor();
    SetRect(&r, (short)(comp_x + 64), 44, (short)(comp_x + 448 - 64), (short)(44 + 24 + n * 13));
    w = NewWindow(NULL, &r, "\p", true, dBoxProc, (WindowPtr)-1, false, 0);
    SetPort(w);
    for (i = 0; i < n; i++) {
        TextFont(i == 0 ? 0 : 3);
        TextSize(i == 0 ? 0 : 9);
        MoveTo(14, (short)(20 + i * 13));
        DrawString(lines[i]);
    }
    for (;;) {
        SystemTask();
        if (GetNextEvent(mDownMask | keyDownMask, &ev))
            break;
    }
    DisposeWindow(w);
    SetPort(win);
    HideCursor();
    render_full(&G);
}

static void set_pause(short on)
{
    if (on == paused)
        return;
    paused = on;
    if (paused) {
        sound_stop();
        ShowCursor();
    } else
        HideCursor();
    update_menus();
}

static void start_human_game(void)
{
    game_start(&G, 0);
    initials_mode = 0;
    show_scores_t = 0;
    sound_start_tune();
}

static void start_attract(void)
{
    game_start(&G, 1);
    initials_mode = 0;
}

static void do_menu(long mr)
{
    short menu = (short)(mr >> 16), item = (short)mr;
    if (menu == M_APPLE) {
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
    } else if (menu == M_FILE) {
        switch (item) {
        case I_NEW: start_human_game(); set_pause(0); break;
        case I_PAUSE: set_pause(!paused); break;
        case I_SOUND:
            sound_set(!sound_is_on());
            scores.sound = sound_is_on();
            scores_save();
            break;
        case I_SPEECH:
            speech_on = !speech_on;
            scores.speech = speech_on;
            scores_save();
            break;
        case I_SCORES: show_scores_t = 600; break;
        case I_QUIT: quitting = 1; break;
        }
        update_menus();
    }
    HiliteMenu(0);
}

static void handle_events(void)
{
    EventRecord ev;
    WindowPtr w;
    while (GetNextEvent(everyEvent, &ev)) {
        switch (ev.what) {
        case mouseDown:
            switch (FindWindow(ev.where, &w)) {
            case inMenuBar:
                ShowCursor();
                update_menus();
                do_menu(MenuSelect(ev.where));
                if (!paused)
                    HideCursor();
                break;
            case inSysWindow:
                SystemClick(&ev, w);
                break;
            default:
                break;
            }
            break;
        case keyDown:
        case autoKey:
            if (ev.modifiers & cmdKey) {
                if (ev.what == keyDown) {
                    update_menus();
                    do_menu(MenuKey((char)(ev.message & charCodeMask)));
                }
            } else if (initials_mode && ev.what == keyDown) {
                char c = (char)(ev.message & charCodeMask);
                if (c >= 'a' && c <= 'z')
                    c = (char)(c - 32);
                if (c >= 'A' && c <= 'Z' && ninit < 3)
                    initials[ninit++] = c;
                else if (c == 8 && ninit > 0)
                    initials[--ninit] = 0;
                else if ((c == 13 || c == 3) && ninit > 0) {
                    scores_insert(initials_rank, G.score, initials);
                    scores_save();
                    initials_mode = 0;
                    show_scores_t = 420;
                    over_since = TICKS;
                }
            }
            break;
        case updateEvt:
            BeginUpdate((WindowPtr)ev.message);
            EndUpdate((WindowPtr)ev.message);
            render_full(&G);
            break;
        case activateEvt:
            break;
        }
    }
}

/* ------------------------------------------------------------------ the panel text */

static long shown_score = -1, shown_at;

static void dmd(void)
{
    char top[MSG_LEN + 8], mid[MSG_LEN], low[MSG_LEN + 8], s[16];
    short big = 0, i;
    long score = G.score;
    /* the score display changes at most 15 times a second (in multiball it would otherwise be
       redrawn every frame) */
    if (score != shown_score) {
        if (dbg.ticks - shown_at >= 4 || shown_score < 0) {
            shown_score = score;
            shown_at = dbg.ticks;
        } else
            score = shown_score;
    }
    top[0] = mid[0] = low[0] = 0;
    if (initials_mode) {
        char t[] = "INITIALS: ___";
        for (i = 0; i < ninit; i++)
            t[10 + i] = initials[i];
        game_message(&G, "", "", 0, 0);
        fmt_score(G.score, mid);
        panel_update(&G, "HIGH SCORE!", mid, t, 1);
        return;
    }
    if (paused) {
        fmt_score(G.score, s);
        panel_update(&G, "PAUSED", "PAUSED", "P TO PLAY ON", 1);
        return;
    }
    if (G.state == GS_PLAY) {
        char b[] = "BALL 1";
        b[5] = (char)('0' + G.ball);
        for (i = 0; b[i]; i++) top[i] = b[i];
        top[i] = 0;
        if (G.ai_playing) {
            const char *a = "  AI PLAYING";
            short k;
            for (k = 0; a[k]; k++) top[i++] = a[k];
            top[i] = 0;
        } else if (G.tilt_warn) {
            const char *a = "  CAREFUL";
            short k;
            for (k = 0; a[k]; k++) top[i++] = a[k];
            top[i] = 0;
        }
    } else
        for (i = 0; "NEURAL PINBALL"[i]; i++) top[i] = "NEURAL PINBALL"[i], top[i + 1] = 0;
    if (G.msg_t > 0 && G.msg_big) {
        for (i = 0; G.msg[0][i]; i++) mid[i] = G.msg[0][i];
        mid[i] = 0;
        for (i = 0; G.msg[1][i]; i++) low[i] = G.msg[1][i];
        low[i] = 0;
        big = 1;
    } else {
        fmt_score(score, mid);
        big = 1;
        if (G.msg_t > 0) {
            short k = 0;
            for (i = 0; G.msg[0][i]; i++) low[k++] = G.msg[0][i];
            if (G.msg[1][0]) {
                low[k++] = ' ';
                for (i = 0; G.msg[1][i] && k < MSG_LEN + 6; i++) low[k++] = G.msg[1][i];
            }
            low[k] = 0;
        } else if (G.state != GS_PLAY)
            for (i = 0; "PRESS RETURN"[i]; i++) low[i] = "PRESS RETURN"[i], low[i + 1] = 0;
        else if (ball_on_plunger() && !G.ai_playing)
            for (i = 0; "HOLD SPACE, LET GO"[i]; i++) low[i] = "HOLD SPACE, LET GO"[i], low[i + 1] = 0;
    }
    panel_update(&G, top, mid, low, big);
}

static void num(char *d, long v)
{
    char t[16];
    short i;
    fmt_score(v, t);
    for (i = 0; t[i]; i++)
        d[i] = t[i];
    d[i] = 0;
}

static unsigned long info_key = 0xFFFFFFFFUL;

static void info(void)
{
    static char l[12][28];
    const char *lines[12];
    short n = 0, i;
    unsigned long k;
    /* a cheap key of everything shown, to redraw only on change */
    k = (unsigned long)G.epoch * 7 + (unsigned long)G.lanes * 131 + (unsigned long)G.ramps * 1031 +
        (unsigned long)G.locks * 7919 + (unsigned long)(G.turing_t / 60) * 104729UL +
        (unsigned long)G.turing_lit * 3 + (unsigned long)G.face * 1299709UL + (unsigned long)G.multiball * 17 +
        (unsigned long)(G.state != GS_PLAY) * 15485863UL + (unsigned long)(show_scores_t > 0) * 32452843UL +
        (unsigned long)G.ai_playing * 49979687UL + (unsigned long)G.turing_hits * 86028121UL +
        (unsigned long)G.lock_lit * 11 + (unsigned long)G.kickback * 13 + (unsigned long)initials_mode * 19;
    if (k == info_key)
        return;
    info_key = k;
    for (i = 0; i < 12; i++)
        l[i][0] = 0, lines[i] = l[i];
    {
        char *p = l[0];
        const char *t = "EPOCH ";
        for (i = 0; t[i]; i++) *p++ = t[i];
        *p++ = (char)('0' + G.epoch);
        *p++ = 'X';
        *p = 0;
    }
    {
        char *p = l[1];
        const char *w = "TRAIN";
        for (i = 0; i < 5; i++) {
            *p++ = (G.lanes & (1 << i)) ? w[i] : '_';
            *p++ = ' ';
        }
        *p = 0;
    }
    {
        char *p = l[2];
        const char *t = "BACKPROP ";
        for (i = 0; t[i]; i++) *p++ = t[i];
        num(p, G.ramps);
    }
    {
        char *p = l[3];
        const char *t = G.multiball ? "MULTIBALL" : G.lock_lit ? "LOCK LIT" : "LOCKS ";
        for (i = 0; t[i]; i++) *p++ = t[i];
        if (!G.multiball && !G.lock_lit) {
            *p++ = (char)('0' + G.locks);
            *p++ = '/';
            *p++ = '2';
        }
        *p = 0;
    }
    {
        char *p = l[4];
        const char *t = G.turing_t ? "TURING " : G.turing_lit ? "TURING LIT" : "TURING TEST";
        for (i = 0; t[i]; i++) *p++ = t[i];
        if (G.turing_t) {
            num(p, G.turing_t / 60);
            while (*p) p++;
            *p++ = 'S';
            *p = 0;
        } else
            *p = 0;
    }
    {
        char *p = l[5];
        const char *t = G.kickback ? "KICKBACK ON" : "";
        for (i = 0; t[i]; i++) *p++ = t[i];
        *p = 0;
    }
    n = 6;
    if (G.state != GS_PLAY || show_scores_t > 0 || G.ai_playing) {
        const char *t = "HIGH SCORES";
        for (i = 0; t[i]; i++) l[6][i] = t[i];
        l[6][i] = 0;
        for (i = 0; i < NSCORES; i++) {
            char *p = l[7 + i];
            *p++ = (char)('1' + i);
            *p++ = '.';
            *p++ = ' ';
            *p++ = scores.e[i].init[0];
            *p++ = scores.e[i].init[1];
            *p++ = scores.e[i].init[2];
            *p++ = ' ';
            num(p, scores.e[i].score);
        }
        n = 12;
    } else {
        static const char *const help[] = {"A/D OR Z,/  FLIPPERS", "SPACE/S  PLUNGER", "W  NUDGE (TILT!)",
                                           "P PAUSE  M SOUND", "ESC  END THE GAME"};
        for (i = 0; i < 5; i++) {
            short k;
            for (k = 0; help[i][k]; k++) l[7 + i][k] = help[i][k];
            l[7 + i][k] = 0;
        }
        n = 12;
    }
    panel_info(lines, n, G.face);
}

/* ------------------------------------------------------------------ main */

static void quit_app(void)
{
    sound_off();
    speech_off();
    FlushEvents(everyEvent, 0);
    ShowCursor();
    ExitToShell();
}

#ifdef HASHTEST
static void hash_test(void)
{
    long i;
    unsigned long t0 = TICKS;
    game_init(&G, 1);
    game_start(&G, 1);
    for (i = 0; i < 60L * 180; i++) {
        unsigned short b = game_ai_buttons(&G);
        short s;
        for (s = 0; s < STEPS_PER_TICK; s++)
            game_step(&G, b);
        game_tick(&G, b);
        if (G.state == GS_OVER)
            game_start(&G, 1);
    }
    dbg.hash = phys_hash(&G.w);
    dbg.score = G.score;
    /* micro-benchmarks: 2000 steps each, the scenario restored before every step */
    {
        static World bw;
        static const Material m[1] = {{115, 800, 20, 0, 0}};
        static const struct { short table, balls; long x, y, vx, vy; short held; } sc[8] = {
            {0, 1, 121L << 16, 200L << 16, 20000, 20000, 0},   /* empty world, slow ball */
            {0, 3, 121L << 16, 200L << 16, 20000, 20000, 0},   /* empty world, 3 balls */
            {1, 1, 121L << 16, 220L << 16, 20000, 20000, 0},   /* table, open area */
            {1, 1, 77L << 16, 268L << 16, 0, 0, 1},            /* table, cradled on the left flipper */
            {1, 1, 121L << 16, 220L << 16, 300000, 300000, 0}, /* table, fast ball */
            {1, 3, 121L << 16, 220L << 16, 20000, 20000, 0},   /* table, 3 balls */
            {1, 1, 121L << 16, 150L << 16, 20000, 20000, 0},   /* table, open area, high */
            {1, 3, 60L << 16, 150L << 16, 120000, -60000, 2},  /* 3 balls, flippers flapping */
        };
        short k, j, i;
        for (k = 0; k < 8; k++) {
            unsigned long t1;
            if (sc[k].table) {
                phys_init(&bw, table_shapes, NSHAPES, table_mats, TABLE_W * 64, TABLE_H * 64);
                phys_add_flipper(&bw, &table_flippers[0]);
                phys_add_flipper(&bw, &table_flippers[1]);
                phys_set_plunger(&bw, PLUNGER_X0, PLUNGER_X1, PLUNGER_Y, PLUNGER_PULL, PLUNGER_RATE, PLUNGER_SPEED);
            } else
                phys_init(&bw, NULL, 0, m, TABLE_W * 64, TABLE_H * 64);
            for (j = 0; j < sc[k].balls; j++)
                phys_add_ball(&bw, sc[k].x + j * (20L << 16), sc[k].y, sc[k].vx, sc[k].vy);
            PHYS_FLIPPER(&bw, 0, sc[k].held);
            for (i = 0; i < 50; i++)
                phys_step(&bw);
            t1 = TICKS;
            for (i = 0; i < 2000; i++) {
                if (sc[k].held == 2) {                 /* flap both flippers: 6 steps up, 6 down */
                    PHYS_FLIPPER(&bw, 0, (i / 6) & 1);
                    PHYS_FLIPPER(&bw, 1, (i / 6) & 1);
                }
                for (j = 0; j < sc[k].balls; j++) {
                    Ball *b = &bw.ball[j];
                    b->state = BALL_FREE;
                    b->x = sc[k].x + j * (20L << 16);
                    b->y = sc[k].y;
                    b->vx = sc[k].vx;
                    b->vy = sc[k].vy;
                }
                bw.nev = 0;
                phys_step(&bw);
            }
            dbg.bench[k] = (long)(TICKS - t1);
        }
    }
    dbg.hash_ticks = (long)(TICKS - t0);
}
#endif

int main(void)
{
    unsigned long last, win_t0;
    long win_f0 = 0, win_k0 = 0;
    dbg.magic = MAGIC;
    dbg.version = 1;
    dbg.notmagic = ~MAGIC;
    {
        long a;
        __asm__ volatile(".globl prof_anchor\nprof_anchor: lea prof_anchor(%%pc),%0" : "=a"(a));
        dbg.anchor = a;
    }
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
    setup_menus();
    {
        Rect r = qd.screenBits.bounds;
        r.top = 20;
        win = NewWindow(NULL, &r, "\p", true, plainDBox, (WindowPtr)-1, false, 0);
        SetPort(win);
    }
#ifdef HASHTEST
    hash_test();
#endif
    scores_load();
    render_init();
    dbg.screen_x = comp_x;
    sound_init();
    sound_set(scores.sound);
    speech_init();
    speech_on = scores.speech;
    dbg.speech = speech_available;
    update_menus();
    game_init(&G, TICKS * 2654435761UL + 1);
    start_attract();
    HideCursor();
    render_full(&G);
    dbg.freemem = FreeMem();
    snap_keys();
    last = TICKS;
    win_t0 = last;
    for (;;) {
        unsigned long now;
        short n, t;
        handle_events();
        if (quitting)
            quit_app();
        if (dbg.cmd == 1) {
            dbg.cmd = 0;
            start_human_game();
        } else if (dbg.cmd == 2) {
            dbg.cmd = 0;
            game_debug_multiball(&G);
        } else if (dbg.cmd == 3) {            /* tests: a high score */
            dbg.cmd = 0;
            G.score += 3000000L;
        }
        now = TICKS;
        n = (short)(now - last);
        if (n <= 0)
            continue;
        if (n > 6) {                    /* (heavy moments cost frames, not game speed) */
            dbg.late += n - 6;
            n = 6;
        }
        last = now;
        for (t = 0; t < n && !spoke; t++) {
            unsigned short b;
            short s;
            snap_keys();
            if (pressed(K_ESC) || pressed(K_GRAVE)) {
                if (G.state == GS_PLAY && !G.ai_playing) {
                    start_attract();
                    set_pause(0);
                } else
                    quit_app();
            }
            if (pressed(K_M)) {
                sound_set(!sound_is_on());
                scores.sound = sound_is_on();
                update_menus();
            }
            if (pressed(K_P) && G.state == GS_PLAY)
                set_pause(!paused);
            if (paused || initials_mode)
                break;
            if ((pressed(K_RETURN) || pressed(K_ENTER)) && (G.state != GS_PLAY || G.ai_playing))
                start_human_game();
            if (G.ai_playing) {
                unsigned short human = read_buttons() & (BTN_LEFT | BTN_RIGHT);
                if (human && G.state == GS_PLAY)
                    game_takeover(&G);
            }
            if (G.ai_playing) {
                b = game_ai_buttons(&G);
                for (s = 0; s < STEPS_PER_TICK; s++)
                    game_step(&G, b);
            } else {
                b = read_buttons();
                for (s = 0; s < STEPS_PER_TICK; s++) {
                    if (s)
                        b = read_buttons();      /* the keys are read before every step (8 ms) */
                    game_step(&G, b);
                }
            }
            dbg.buttons = b;
            game_tick(&G, b);
            sound_events(G.sounds);
            G.sounds = 0;
            sound_tick();
            if (G.say) {
                /* MacinTalk blocks while it talks, so only big moments are spoken, in a player's
                   game; the table pauses meanwhile (the clock is resynchronised below) */
                short id = G.say;
                G.say = 0;
                if (speech_on && speech_available && !G.ai_playing &&
                    (id == SAY_MULTIBALL || id == SAY_JACKPOT || id == SAY_SINGULARITY || id == SAY_TURING ||
                     id == SAY_PASSED || id == SAY_FAILED || id == SAY_TILT || id == SAY_SAVED ||
                     id == SAY_GAMEOVER)) {
                    render_frame(&G);
                    dmd();
                    panel_present();
                    sound_stop();
                    speech_say(id);
                    sound_reset();
                    dbg.spoken++;
                    spoke = 1;
                }
            }
            dbg.ticks++;
            if (show_scores_t > 0)
                show_scores_t--;
            /* game over: high score entry for players, then the AI plays again */
            if (G.state == GS_OVER) {
                if (!over_since)
                    over_since = TICKS;
                if (!G.ai_playing && !initials_mode && !G.new_hiscore) {
                    G.new_hiscore = 1;
                    initials_rank = scores_rank(G.score);
                    if (initials_rank >= 0) {
                        initials_mode = 1;
                        ninit = 0;
                        initials[0] = initials[1] = initials[2] = initials[3] = 0;
                        FlushEvents(keyDownMask | autoKeyMask, 0);
                    } else
                        show_scores_t = 300;
                }
                if (!initials_mode && TICKS - over_since > 420) {
                    over_since = 0;
                    start_attract();
                }
            } else
                over_since = 0;
        }
        if (spoke) {                    /* the game paused while the Mac talked */
            last = TICKS;
            spoke = 0;
        }
        if (paused || initials_mode)
            sound_tick();
        render_frame(&G);
        dmd();
        info();
        panel_present();
        dbg.frames++;
        dbg.mode = initials_mode ? 5 : paused ? 4 : G.state == GS_PLAY ? (G.ai_playing ? 1 : 2) : 3;
        dbg.score = G.score;
        dbg.ball = G.ball;
        dbg.gstate = G.state;
        dbg.ai = G.ai_playing;
        dbg.inplay = G.in_play;
        dbg.face = G.face;
        dbg.copied = render_stats();
        dbg.top_init[0] = scores.e[0].init[0];
        dbg.top_init[1] = scores.e[0].init[1];
        dbg.top_init[2] = scores.e[0].init[2];
        dbg.top_score = scores.e[0].score;
        {
            short i;
            for (i = 0; i < MAX_BALLS; i++) {
                dbg.bx[i] = G.w.ball[i].state ? (short)(G.w.ball[i].x >> 16) : -1;
                dbg.by[i] = G.w.ball[i].state ? (short)(G.w.ball[i].y >> 16) : -1;
            }
            dbg.flipf[0] = (short)(G.w.flip[0].ang >> 4);
            dbg.flipf[1] = (short)(G.w.flip[1].ang >> 4);
        }
        if (TICKS - win_t0 >= 120) {
            unsigned long dt = TICKS - win_t0;
            dbg.fps100 = (dbg.frames - win_f0) * 6015L / (long)dt;
            dbg.tps100 = (dbg.ticks - win_k0) * 6015L / (long)dt;
            win_t0 = TICKS;
            win_f0 = dbg.frames;
            win_k0 = dbg.ticks;
        }
    }
    return 0;
}

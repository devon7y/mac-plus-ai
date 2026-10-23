/* Rail Runner for the Macintosh Plus: main loop, input, screens, debug block.
 *
 * Game logic runs at a fixed 60.15 Hz (one game_tick per Mac tick, from Ticks at $16A);
 * rendering runs as fast as it can: background rows, objects, player into an offscreen
 * playfield, then one copy to the screen centred in screenBits.bounds.
 *
 * Build flags: -DBENCH (M1 frame-rate benchmark: invulnerable autopilot, then timings of each
 * stage), -DREPLAY (plays the input table in replay.h: deterministic test of the game logic).
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
#include <Multiverse.h>   /* (not <Sound.h>: it would find our sound.h on a case-insensitive disk) */
#include "render.h"
#include "sound.h"
#include "hiscore.h"
#ifdef REPLAY
#include "replay.h"
#endif

#define TICKS (*(volatile unsigned long *)(long)ticks_addr)
static volatile long ticks_addr = 0x16A;
#define MAGIC 0x5252554EUL           /* 'RRUN' */
#define HUD_H 22

/* ------------------------------------------------------------------ debug block (Lua reads it) */
struct Dbg {
    unsigned long magic, version, notmagic;   /*  0 */
    long mode;                                /* 12: 0 init 1 title 2 play 3 over 4 bench done */
    long frames;                              /* 16: frames rendered in play */
    long ticks;                               /* 20: Ticks now */
    long fps100;                              /* 24: bench: frames per second x 100 */
    long win_frames, win_ticks;               /* 28, 32: bench window */
    long t_bg, t_obj, t_blit, t_logic;        /* 36..48: bench: ticks for BENCH_N runs of each */
    long score, coins, z;                     /* 52, 56, 60 */
    short gstate, pf_w, pf_h, pf_left, pf_top;/* 64..72 */
    unsigned char keys[16];                   /* 74 */
    long freemem;                             /* 90 */
    short lane, y, nobj, ncoin;               /* 94..100 */
    long game_ticks;                          /* 102 */
    long replay_pos, replay_len;              /* 106, 110 */
    long best;                                /* 114 */
    short runs;                               /* 118 */
    short snd_on;                             /* 120 */
    long t_snd;                               /* 122 */
    long t_part[6];                           /* 126: bench: poles blocks faces coins player none */
    long fn_render_objects, fn_main;          /* 150, 154: runtime addresses (profiler anchors) */
    long prof[8];                             /* 158: PROFILE counters for one render_objects */
    long cp[16][4];                           /* 190: REPLAY checkpoints every 600 ticks: score, coins, z, state<<16|lane<<8 + y>>4 */
    short ncp;                                /* 446 */
    short roll, hover, boards;                /* 448, 450, 452: roll ticks left, hoverboard ticks, boards */
} dbg;

static unsigned char *scr;          /* top-left of screenBits */
static short srb;                   /* screen row bytes */
static short bw, bh;                /* screenBits size */
static short pf_x, pf_y;            /* playfield position (px) in screenBits */
static Game game;
static Surface hud;                 /* the screen strip above the playfield */
static unsigned char km[16], km_prev[16];
static long best_score;

/* ------------------------------------------------------------------ input */
#define KEY(c) ((km[(c) >> 3] >> ((c) & 7)) & 1)
#define WAS(c) ((km_prev[(c) >> 3] >> ((c) & 7)) & 1)
#define PRESSED(c) (KEY(c) && !WAS(c))

static short queue[8], qn;

static void poll_keys(void)
{
    short i;
    for (i = 0; i < 16; i++)
        km_prev[i] = km[i];
    GetKeys(km);
    for (i = 0; i < 16; i++)
        dbg.keys[i] = km[i];
}

static void push(short in)
{
    if (qn < 8)
        queue[qn++] = in;
}

/* Standard keys (DECISIONS.md, 2026-10-16): A / D change lanes, W jumps, S rolls, Space = hoverboard.
 * Alternates: arrows (Plus keyboard 0x46 left, 0x42 right, 0x4D up, 0x48 down; ADB 0x7B..0x7E),
 * X jumps, Z rolls. Virtual key codes are the same on the Plus and SE keyboards. Each key is
 * edge-detected on its own, so any keys may be held together. */
#define K_A 0x00
#define K_S 0x01
#define K_D 0x02
#define K_W 0x0D
#define K_M 0x2E
#define K_Q 0x0C
#define K_SPACE 0x31
#define QUIT_KEYS ((KEY(0x37) || KEY(0x3B)) && PRESSED(K_Q))   /* Command-Q (or Control-Q on ADB) */
/* Esc: the SE's Esc key (0x35) or the Plus's ` key (0x32), which emu/play.sh maps the host's Esc to */
#define ESC_PRESSED (PRESSED(0x35) || PRESSED(0x32))
static void read_game_keys(void)
{
    if (PRESSED(K_A) || PRESSED(0x46) || PRESSED(0x7B)) push(IN_LEFT);
    if (PRESSED(K_D) || PRESSED(0x42) || PRESSED(0x7C)) push(IN_RIGHT);
    if (PRESSED(K_W) || PRESSED(0x4D) || PRESSED(0x7E) || PRESSED(0x07)) push(IN_JUMP);
    if (PRESSED(K_S) || PRESSED(0x48) || PRESSED(0x7D) || PRESSED(0x06)) push(IN_ROLL);
    if (PRESSED(K_SPACE)) push(IN_BOARD);
}

static short pop(void)
{
    short v, i;
    if (!qn)
        return IN_NONE;
    v = queue[0];
    for (i = 1; i < qn; i++)
        queue[i - 1] = queue[i];
    qn--;
    return v;
}

/* mouse swipes: a drag of more than 12 px while the button is down */
static Point swipe_start;
static short swiping;
static void read_swipe(void)
{
    Point m;
    if (!Button()) {
        swiping = 0;
        return;
    }
    GetMouse(&m);
    if (!swiping) {
        swiping = 1;
        swipe_start = m;
        return;
    }
    if (swiping == 1) {
        short dx = m.h - swipe_start.h, dy = m.v - swipe_start.v;
        short ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        if (ax > 12 || ay > 12) {
            if (ax > ay) push(dx < 0 ? IN_LEFT : IN_RIGHT);
            else push(dy < 0 ? IN_JUMP : IN_ROLL);
            swiping = 2;            /* one action per drag */
        }
    }
}

/* ------------------------------------------------------------------ screen helpers */
static void screen_fill(short y0, short y1, unsigned char v)
{
    short y, x;
    for (y = y0; y < y1; y++) {
        unsigned char *r = scr + (long)y * srb;
        for (x = 0; x < (bw >> 3); x++)
            r[x] = v;
    }
}

static void blit_playfield(void)
{
    blit_to_screen(scr + (long)pf_y * srb + (pf_x >> 3), srb);
}

static void draw_text(short x, short y, const unsigned char *pstr, short white)
{
    TextMode(white ? srcBic : srcOr);
    MoveTo(x, y);
    DrawString(pstr);
    TextMode(srcOr);
}

static void draw_centred(short y, const unsigned char *pstr)
{
    draw_text(pf_x + PF_W / 2 - StringWidth(pstr) / 2, y, pstr, 1);
}

static void num_to_pstr(long v, unsigned char *s)
{
    NumToString(v, s);
}

/* ------------------------------------------------------------------ HUD */
static long hud_score = -1, hud_coins = -1;
static short hud_key = -1;

/* icon + remaining-time bar for an active power-up */
static void hud_power(short x, short spr, short t, short tmax)
{
    short w;
    draw_sprite(&hud, spr, x + 9, 16);
    w = (short)((long)t * 18 / tmax);
    fill_rect(&hud, x, 18, x + 18, 19, ART_WHITE);
    if (w > 0)
        fill_rect(&hud, x, 18, x + w, 19, ART_BLACK);
}

static void hud_draw(int force)
{
    short key = (game.jet_t >> 5) * 1 + (game.sneak_t >> 5) * 3 + (game.magnet_t >> 5) * 7 +
                (game.mult_t >> 5) * 11 + (game.hover_t >> 5) * 13 + game.boards * 101;
    short x;
    static short skip;
    if (!force && game.score == hud_score && game.coins == hud_coins && key == hud_key)
        return;
#ifdef HAVE_FONT_0
    if (!force && key == hud_key) {
        /* only the numbers moved: redraw just their digits (the score every 4th frame) */
        if (game.coins != hud_coins) {
            fill_rect(&hud, PF_W - 48, 0, PF_W - 1, HUD_H - 2, ART_WHITE);
            draw_number(&hud, PF_W - 48, 5, game.coins, 4);
            hud_coins = game.coins;
        }
        if (game.score != hud_score && !(++skip & 3)) {
            fill_rect(&hud, 0, 0, 88, HUD_H - 2, ART_WHITE);   /* power-up icons start at x = 92 */
            draw_number(&hud, 6, 5, game.score, 6);
            hud_score = game.score;
        }
        return;
    }
#endif
    fill_rect(&hud, 0, 0, PF_W - 1, HUD_H - 1, ART_WHITE);
    fill_rect(&hud, 0, HUD_H - 1, PF_W - 1, HUD_H - 1, ART_BLACK);
#ifdef HAVE_FONT_0
    draw_number(&hud, 6, 5, game.score, 6);
#endif
#ifdef HAVE_HUD_COIN
    draw_sprite(&hud, SPR_HUD_COIN, PF_W - 62, 5);
#endif
#ifdef HAVE_FONT_0
    draw_number(&hud, PF_W - 48, 5, game.coins, 4);
#endif
    x = 92;
#ifdef HAVE_PU_JETPACK_0
    if (game.jet_t)    { hud_power(x, SPR_PU_JETPACK_0 + 2, game.jet_t, JET_T); x += 24; }
    if (game.sneak_t)  { hud_power(x, SPR_PU_SNEAKERS_0 + 2, game.sneak_t, SNEAKERS_T); x += 24; }
    if (game.magnet_t) { hud_power(x, SPR_PU_MAGNET_0 + 2, game.magnet_t, MAGNET_T); x += 24; }
    if (game.mult_t)   { hud_power(x, SPR_PU_MULT_0 + 2, game.mult_t, MULT_T); x += 24; }
    if (game.hover_t)  { hud_power(x, SPR_PU_BOARD_0 + 2, game.hover_t, HOVER_T); x += 24; }
#endif
#ifdef HAVE_HUD_BOARD
    {
        short i;
        for (i = 0; i < game.boards && i < 5; i++)
            draw_sprite(&hud, SPR_HUD_BOARD, PF_W - 150 + i * 16, 7);
    }
#endif
    hud_score = game.score;
    hud_coins = game.coins;
    hud_key = key;
}

/* Quit: silence and disable sound, give the cursor back, and leave. On the game floppy the
   boot blocks name Rail Runner as the startup application, so the Finder never appears. */
static void quit_app(void)
{
    sound_off();
    FlushEvents(everyEvent, 0);
    ShowCursor();
    ExitToShell();
}

/* ------------------------------------------------------------------ title and game over */
static void draw_title(void)
{
    unsigned char s[32];
    screen_fill(0, bh, 0xFF);
    render_invalidate();
    render_background(0);
#ifdef HAVE_TITLE_LOGO
    draw_sprite(&pf, SPR_TITLE_LOGO, ((PF_W - spr_defs[SPR_TITLE_LOGO].w) / 2) & ~15, 6);
#endif
#ifdef HAVE_TITLE_REX
    draw_sprite(&pf, SPR_TITLE_REX, PF_W / 2, PF_H - 8);
#else
    draw_sprite(&pf, SPR_REX_RUN0, PF_W / 2, PF_FEET);
#endif
#ifdef HAVE_GRUMBLE_RUN0
    draw_sprite(&pf, SPR_GRUMBLE_RUN0, PF_W / 2 - 70, PF_H + 18);
#endif
#ifdef HAVE_BISCUIT_RUN0
    draw_sprite(&pf, SPR_BISCUIT_RUN0, PF_W / 2 + 60, PF_H - 2);
#endif
    render_invalidate();
    blit_playfield();
    TextFont(0);
    TextSize(12);
    draw_centred(pf_y + PF_H + 18, "\pPress Return to run!");
    if (pf_y >= 40) {
        draw_centred(pf_y - 22, "\pA, D: change lanes     W: jump     S: roll");
        draw_centred(pf_y - 6, "\pSpace: hoverboard     (arrow keys work too)");
    } else {
        draw_centred(pf_y - 6, "\pA D: lanes   W: jump   S: roll   Space: board");
    }
    if (pf_y + PF_H + 36 < bh)
        draw_centred(pf_y + PF_H + 36, "\pM: sound on/off      Esc: quit");
    if (best_score > 0) {
        num_to_pstr(best_score, s);
        draw_text(pf_x + 4, pf_y + PF_H + 18, "\pBest", 1);
        draw_text(pf_x + 40, pf_y + PF_H + 18, s, 1);
    }
}

static void draw_game_over(short new_best)
{
    unsigned char s[32];
    short cx = pf_x + PF_W / 2, cy = pf_y + PF_H / 2;
    Rect r;
    SetRect(&r, cx - 110, cy - 46, cx + 110, cy + 50);
    FillRect(&r, &qd.white);
    FrameRect(&r);
    InsetRect(&r, 3, 3);
    FrameRect(&r);
    TextFont(0);
    TextSize(12);
    draw_text(cx - 40, cy - 24, game.state == ST_CAUGHT ? "\pBUSTED!" : "\pCRASH!", 0);
    draw_text(cx - 90, cy - 2, "\pScore", 0);
    num_to_pstr(game.score, s);
    draw_text(cx - 20, cy - 2, s, 0);
    draw_text(cx - 90, cy + 16, "\pCoins", 0);
    num_to_pstr(game.coins, s);
    draw_text(cx - 20, cy + 16, s, 0);
    draw_text(cx - 90, cy + 34, new_best ? "\pNew best!" : "\pBest", 0);
    num_to_pstr(best_score, s);
    draw_text(cx - 20, cy + 34, s, 0);
}

/* ------------------------------------------------------------------ play */
static unsigned long seed_for_run(void)
{
#if defined(BENCH) || defined(REPLAY)
    return 0x2545F491UL;
#else
    return TICKS * 2654435761UL + 0x2545F491UL;
#endif
}

#ifdef BENCH
#define BENCH_N 30
#ifndef BENCH_WINDOW
#define BENCH_WINDOW 1200
#endif
static void run_bench(void)
{
    unsigned long t0, last, now;
    long f0 = 0;
    short i;
    game_init(&game, seed_for_run());
    game.invuln_t = 30000;
    last = TICKS;
    t0 = 0;
    for (;;) {
        short n;
        now = TICKS;
        n = (short)(now - last);
        if (n > 4) n = 4;
        last = now;
        while (n-- > 0) {
            game.invuln_t = 30000;
            game_tick(&game, IN_NONE);
            if (game.chase_t) game.chase_t = 0;
        }
        render_frame(&game);
        blit_playfield();
        dbg.frames++;
        dbg.ticks = TICKS;
        dbg.z = game.z;
        dbg.nobj = game.nobj;
        dbg.ncoin = game.ncoin;
        if (game.ticks >= 300 && !t0) {
            t0 = TICKS;
            f0 = dbg.frames;

        }
        if (t0 && TICKS - t0 >= BENCH_WINDOW)
            break;
    }
    dbg.win_ticks = TICKS - t0;
    dbg.win_frames = dbg.frames - f0;
    dbg.fps100 = dbg.win_frames * 6015L / dbg.win_ticks;
    /* stage timings on the current scene */
    t0 = TICKS;
    for (i = 0; i < BENCH_N; i++) {
        render_invalidate();
        render_background(game.z - ZP);
    }
    dbg.t_bg = TICKS - t0;
    t0 = TICKS;
    for (i = 0; i < BENCH_N; i++)
        render_objects(&game);
    dbg.t_obj = TICKS - t0;
    {
        extern short render_mask;
        short k;
        static const short masks[5] = {1, 2 | 32 | 64 | 128 | 256, 4, 8, 16};
        for (k = 0; k < 5; k++) {
            render_mask = masks[k];
            t0 = TICKS;
            for (i = 0; i < BENCH_N; i++)
                render_objects(&game);
            dbg.t_part[k] = TICKS - t0;
        }
        render_mask = 0;
        t0 = TICKS;
        for (i = 0; i < BENCH_N; i++)
            render_objects(&game);
        dbg.t_part[5] = TICKS - t0;
        render_mask = 0x7FFF;
#ifdef PROFILE
        {
            extern long prof_cnt[8];
            for (k = 0; k < 8; k++) prof_cnt[k] = 0;
            render_objects(&game);
            for (k = 0; k < 8; k++) dbg.prof[k] = prof_cnt[k];
        }
#endif
    }
    t0 = TICKS;
    for (i = 0; i < BENCH_N; i++) {
        render_invalidate();
        blit_playfield();
    }
    dbg.t_blit = TICKS - t0;
    t0 = TICKS;
    for (i = 0; i < BENCH_N * 4; i++)
        game_tick(&game, IN_NONE);
    dbg.t_logic = TICKS - t0;
    dbg.mode = 4;
    for (;;)
        dbg.ticks = TICKS;
}
#endif

static void play(void)
{
    unsigned long last, now;
    short over_drawn = 0;
#ifdef REPLAY
    long rp = 0;
    dbg.replay_len = REPLAY_LEN;
#endif
    game_init(&game, seed_for_run());
    screen_fill(0, bh, 0xFF);
    render_invalidate();
    hud_draw(1);
    dbg.mode = 2;
    dbg.runs++;
    qn = 0;
    poll_keys();
    last = TICKS;
    for (;;) {
        short n;
        poll_keys();
        read_game_keys();
        read_swipe();
        if (QUIT_KEYS)                                             /* Command-Q: quit */
            quit_app();
        if (ESC_PRESSED)                                           /* Esc: back to the title */
            return;
        if (PRESSED(K_M)) {                                        /* M: sound on/off */
            sound_toggle();
            dbg.snd_on = !sound_muted();
        }
        now = TICKS;
        n = (short)(now - last);
        if (n > 4) n = 4;                   /* after a long stall, slow down rather than jump */
        last = now;
        while (n-- > 0) {
            short in = pop();
#ifdef REPLAY
            in = IN_NONE;
            while (rp < REPLAY_LEN && replay_tick[rp] == game.ticks + 1) {
                in = replay_in[rp];
                rp++;
            }
            dbg.replay_pos = rp;
#endif
            game_tick(&game, in);
            sound_events(game.events);
#ifdef REPLAY
            if (game.ticks % 600 == 0 && dbg.ncp < 16) {
                long *c = dbg.cp[dbg.ncp++];
                c[0] = game.score;
                c[1] = game.coins;
                c[2] = game.z;
                c[3] = ((long)game.state << 16) | ((long)game.p.lane << 8) | (game.p.y >> 4);
            }
#endif
        }
        sound_update(&game);
        render_frame(&game);
        blit_playfield();
        hud_draw(0);
        dbg.frames++;
        dbg.ticks = TICKS;
        dbg.score = game.score;
        dbg.coins = game.coins;
        dbg.z = game.z;
        dbg.gstate = game.state;
        dbg.lane = game.p.lane;
        dbg.y = game.p.y;
        dbg.nobj = game.nobj;
        dbg.ncoin = game.ncoin;
        dbg.game_ticks = game.ticks;
        dbg.roll = game.p.roll;
        dbg.hover = game.hover_t;
        dbg.boards = game.boards;
        if (game.state != ST_RUN) {
            if (game.state_t > 90 && !over_drawn) {
                short new_best = game.score > best_score;
                if (new_best) {
                    hiscore_save(game.score);
                    best_score = game.score;
                }
                draw_game_over(new_best);
                dbg.best = best_score;
                over_drawn = 1;
                dbg.mode = 3;
                sound_stop();
            }
            if (over_drawn) {
                /* wait for Return / Space / click, then back to the title */
                for (;;) {
                    poll_keys();
                    sound_tick();
                    dbg.ticks = TICKS;
                    if (QUIT_KEYS)
                        quit_app();
                    if (PRESSED(K_M)) {
                        sound_toggle();
                        dbg.snd_on = !sound_muted();
                    }
                    if (PRESSED(0x24) || PRESSED(0x31) || PRESSED(0x4C) || ESC_PRESSED || Button())
                        break;
                }
                while (Button())
                    ;
                return;
            }
        }
    }
}

int main(void)
{
    dbg.magic = MAGIC;
    dbg.version = 1;
    dbg.notmagic = ~MAGIC;
    {
        long a;
        __asm__ volatile(".globl prof_anchor\nprof_anchor: lea prof_anchor(%%pc),%0" : "=a"(a));
        dbg.fn_render_objects = a;
        dbg.fn_main = a;
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
    HideCursor();
    FlushEvents(everyEvent, 0);

    scr = (unsigned char *)qd.screenBits.baseAddr;
    srb = qd.screenBits.rowBytes;
    bw = qd.screenBits.bounds.right - qd.screenBits.bounds.left;
    bh = qd.screenBits.bounds.bottom - qd.screenBits.bounds.top;
    pf_x = ((bw - PF_W) / 2) & ~15;
    pf_y = (bh - PF_H - HUD_H) / 2 + HUD_H;
    dbg.pf_w = PF_W;
    dbg.pf_h = PF_H;
    dbg.pf_left = pf_x;
    dbg.pf_top = pf_y;

    render_init();
    hud.base = (unsigned short *)(scr + (long)(pf_y - HUD_H) * srb + (pf_x >> 3));
    hud.rw = srb / 2;
    hud.w = PF_W;
    hud.h = HUD_H;
    sound_init();
    dbg.snd_on = !sound_muted();
    best_score = hiscore_load();
    dbg.best = best_score;
    dbg.freemem = FreeMem();

#ifdef BENCH
    dbg.mode = 2;
    screen_fill(0, bh, 0xFF);
    run_bench();
#endif
    for (;;) {
        short go = 0;
        dbg.mode = 1;
        draw_title();
        sound_title();
        while (!go) {
            poll_keys();
            sound_tick();
            dbg.ticks = TICKS;
            if (QUIT_KEYS || ESC_PRESSED)            /* Esc or Command-Q on the title: quit */
                quit_app();
            if (PRESSED(K_M)) {                      /* M: sound on/off */
                sound_toggle();
                dbg.snd_on = !sound_muted();
            }
            if (PRESSED(0x24) || PRESSED(0x4C) || PRESSED(0x31))
                go = 1;
            if (Button()) {
                while (Button())
                    ;
                go = 1;
            }
#ifdef REPLAY
            go = 1;
#endif
        }
        play();
    }
    return 0;
}

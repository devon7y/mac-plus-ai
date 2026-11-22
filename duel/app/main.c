/* Orbit Duel: a Spacewar!-style duel around a star, for the Macintosh Plus and SE.
 *
 * Screens: the title (with a computer-vs-computer demo behind it), a game, and the link screens
 * (AppleTalk opponent chooser, serial cable). A game is always the same deterministic simulation
 * (sim.c) stepped 30 times a second of game time; the two players' inputs come from the keyboard,
 * the computer pilot, or the other Mac (lockstep, link.c). Drawing goes through render.c
 * (offscreen buffers, dirty rectangles, straight to the screen).
 *
 * A debug block in the globals ('ODUL') lets the MAME test scripts read the state and send
 * commands (cmd/arg) and switch on an autopilot for the local ship.
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
#include <Resources.h>
#include <SegLoad.h>
#include <Devices.h>
#include <string.h>
#include "sim.h"
#include "render.h"
#include "sound.h"
#include "link.h"
#include "main.h"

#define TICKS (*(volatile unsigned long *)(long)ticks_addr)
static volatile long ticks_addr = 0x16A;

Dbg dbg;

/* ---- screens and match state ------------------------------------------------------------ */
enum { SCR_TITLE = 1, SCR_GAME = 2, SCR_LINK = 3 };
static short screen;
static short kind;                  /* K_ATTRACT, K_SOLO, K_HOTSEAT, K_NET */
static short local_ship;            /* K_NET: which ship this Mac flies (0 = host = Needle) */
static Sim sim;
static Ai ai[2];
static short ai_level = 1;          /* the computer pilot's level (Options menu, Tab on the title) */
static unsigned long t0;            /* Ticks at sim tick 0 (pacing) */
static short gframe;                 /* rendered frames (animation phase) */
static short leaving;               /* Esc pressed during a game */
static unsigned long last_draw;     /* Ticks of the last game frame drawn */

static WindowPtr win;
static MenuHandle mApple, mFile, mOpt;
enum { M_APPLE = 128, M_FILE = 129, M_OPT = 130 };
enum { I_SOLO = 1, I_HOTSEAT, I_ATALK, I_SERIAL, I_SEP1, I_QUIT };
enum { I_SOUND = 1, I_SEP2, I_EASY, I_NORMAL, I_HARD };

/* ---- keyboard ------------------------------------------------------------------------- */
static unsigned char km[16], km_prev[16];
#define KEY(c) ((km[(c) >> 3] >> ((c) & 7)) & 1)
#define WAS(c) ((km_prev[(c) >> 3] >> ((c) & 7)) & 1)
#define PRESSED(c) (KEY(c) && !WAS(c))
#define K_A 0x00
#define K_S 0x01
#define K_D 0x02
#define K_W 0x0D
#define K_I 0x22
#define K_J 0x26
#define K_K 0x28
#define K_L 0x25
#define K_M 0x2E
#define K_P 0x23
#define K_SPACE 0x31
#define K_RETURN 0x24
#define K_ENTER 0x4C
#define K_TAB 0x30
#define K_SHIFT 0x38
#define K_OPTION 0x3A
#define K_COMMAND 0x37
#define K_CONTROL 0x3B
#define K_RSHIFT 0x3C
/* arrows: the Plus keyboard sends keypad-style codes, ADB keyboards their own */
#define K_LEFT (KEY(0x46) || KEY(0x7B))
#define K_RIGHT (KEY(0x42) || KEY(0x7C))
#define K_UP (KEY(0x4D) || KEY(0x7E))
#define K_DOWN (KEY(0x48) || KEY(0x7D))
/* Esc: the SE's Esc key (0x35) or the Plus's ` key (0x32), which emu/play.sh maps the host's Esc to */
#define ESC_PRESSED (PRESSED(0x35) || PRESSED(0x32))

void keys_poll(void)
{
    short i;
    for (i = 0; i < 16; i++)
        km_prev[i] = km[i];
    GetKeys(km);
}

int esc_pressed(void) { return ESC_PRESSED; }
int key_hit(int code) { return PRESSED(code); }

/* one player using the whole keyboard (vs the computer, or a network game) */
static unsigned char keys_single(void)
{
    unsigned char in = 0;
    if (KEY(K_A) || K_LEFT) in |= IN_LEFT;
    if (KEY(K_D) || K_RIGHT) in |= IN_RIGHT;
    if (KEY(K_W) || K_UP) in |= IN_THRUST;
    if (KEY(K_SPACE) || KEY(K_SHIFT) || KEY(K_RSHIFT) || KEY(K_OPTION) || KEY(K_COMMAND)) in |= IN_FIRE;
    if (KEY(K_S) || K_DOWN) in |= IN_HYPER;
    if (KEY(K_RETURN) || KEY(K_P) || KEY(K_ENTER)) in |= IN_START;
    return in;
}

/* two players on one keyboard: Needle = W A S D + Space / Option / Command,
   Wedge = I J K L (or arrows) + Shift / Enter. Return or P pauses. */
static void keys_hotseat(unsigned char *in0, unsigned char *in1)
{
    unsigned char a = 0, b = 0;
    if (KEY(K_A)) a |= IN_LEFT;
    if (KEY(K_D)) a |= IN_RIGHT;
    if (KEY(K_W)) a |= IN_THRUST;
    if (KEY(K_S)) a |= IN_HYPER;
    if (KEY(K_SPACE) || KEY(K_OPTION) || KEY(K_COMMAND)) a |= IN_FIRE;
    if (KEY(K_RETURN) || KEY(K_P)) a |= IN_START;
    if (KEY(K_J) || K_LEFT) b |= IN_LEFT;
    if (KEY(K_L) || K_RIGHT) b |= IN_RIGHT;
    if (KEY(K_I) || K_UP) b |= IN_THRUST;
    if (KEY(K_K) || K_DOWN) b |= IN_HYPER;
    if (KEY(K_SHIFT) || KEY(K_RSHIFT) || KEY(K_ENTER)) b |= IN_FIRE;
    *in0 = a;
    *in1 = b;
}

/* the local player's input for a network game (or the autopilot in tests) */
unsigned char local_input(void)
{
    if (dbg.autopilot == 1)
        return ai_input(&ai[local_ship], &sim, local_ship);
    return keys_single();
}

/* ---- events ----------------------------------------------------------------------------- */
static void about(void);
static void do_menu(long mr);
static void start_game(short k, unsigned long seed, short ship);
static void title_enter(void);

static short mbar_h(void)
{
    short h = *(volatile short *)(long)0xBAA;      /* MBarHeight */
    return h > 0 && h < 64 ? h : 20;
}

static short ui_dirty;          /* redraw everything at the next frame */
static short cursor_on = 1;     /* InitCursor shows it */

static void cursor_hide(void) { if (cursor_on) { HideCursor(); cursor_on = 0; } }
static void cursor_show(void) { if (!cursor_on) { ShowCursor(); cursor_on = 1; } }

/* redraw the whole screen from bg without leaving cursor droppings */
void full_redraw(void)
{
    short was = cursor_on;
    cursor_hide();
    r_full();
    if (was)
        cursor_show();
}
short front_is_ours(void) { return FrontWindow() == win; }

/* Handles the Toolbox events of every screen. Returns the character of a key press (or 0);
   *click gets mouse clicks in our window (global coordinates). */
short poll_events(Point *click)
{
    EventRecord ev;
    short key = 0;
    WindowPtr w;
    SystemTask();
    while (GetNextEvent(everyEvent, &ev)) {
        switch (ev.what) {
        case mouseDown:
            switch (FindWindow(ev.where, &w)) {
            case inMenuBar:
                if (screen != SCR_GAME) {
                    do_menu(MenuSelect(ev.where));
                    ui_dirty = 1;
                }
                break;
            case inSysWindow:
                SystemClick(&ev, w);
                break;
            case inContent:
                if (w != FrontWindow()) {
                    SelectWindow(w);
                    ui_dirty = 1;
                } else if (click)
                    *click = ev.where;
                break;
            default:
                break;
            }
            break;
        case keyDown:
        case autoKey:
            if ((ev.modifiers & cmdKey) && screen != SCR_GAME) {
                do_menu(MenuKey((char)(ev.message & charCodeMask)));
                ui_dirty = 1;
            } else if (ev.what == keyDown)
                key = (short)(ev.message & charCodeMask);
            break;
        case updateEvt:
            BeginUpdate((WindowPtr)ev.message);
            EndUpdate((WindowPtr)ev.message);
            if ((WindowPtr)ev.message == win)
                ui_dirty = 1;
            break;
        case activateEvt:
            if ((WindowPtr)ev.message == win && (ev.modifiers & activeFlag))
                ui_dirty = 1;
            break;
        default:
            break;
        }
    }
    return key;
}

/* one frame of the star alone, with the cursor shown (link screens) */
void present_glow(short f)
{
    Rect sr;
    SetRect(&sr, r_view_left, r_view_top, r_view_left + ARENA_W, r_view_top + ARENA_H);
    cursor_show();
    ShieldCursor(&sr, (Point){0, 0});
    r_begin();
    r_glow(f);
    r_end();
    ShowCursor();
}

/* ---- menus, About ------------------------------------------------------------------------ */
static void set_level(short l)
{
    short i;
    ai_level = l;
    for (i = 0; i < 3; i++)
        CheckItem(mOpt, I_EASY + i, i == l);
    dbg.ai_level = l;
}

static void set_sound(short on)
{
    sound_set_muted(!on);
    CheckItem(mOpt, I_SOUND, on);
}

static void quit(void)
{
    link_close();
    sound_off();
    cursor_show();
    ExitToShell();
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
        if (item == I_SOLO) start_game(K_SOLO, TICKS, 0);
        else if (item == I_HOTSEAT) start_game(K_HOTSEAT, TICKS, 0);
        else if (item == I_ATALK) link_screen(LINK_ATALK);
        else if (item == I_SERIAL) link_screen(LINK_SERIAL);
        else if (item == I_QUIT) quit();
        break;
    case M_OPT:
        if (item == I_SOUND) set_sound(sound_muted());
        else if (item >= I_EASY) set_level(item - I_EASY);
        break;
    }
    HiliteMenu(0);
}

static void setup_menus(void)
{
    mApple = NewMenu(M_APPLE, "\p\x14");
    AppendMenu(mApple, "\pAbout Orbit Duel\xC9;(-");
    AppendResMenu(mApple, 'DRVR');
    InsertMenu(mApple, 0);
    mFile = NewMenu(M_FILE, "\pGame");
    AppendMenu(mFile, "\pPlay the Computer/1;Two Players Here/2;AppleTalk Game\xC9/3;"
                      "Serial Cable Game\xC9/4;(-;Quit/Q");
    InsertMenu(mFile, 0);
    mOpt = NewMenu(M_OPT, "\pOptions");
    AppendMenu(mOpt, "\pSound/M;(-;Computer: Easy;Computer: Normal;Computer: Hard");
    InsertMenu(mOpt, 0);
    DrawMenuBar();
}

static void about(void)
{
    WindowPtr w;
    Rect r, b = qd.screenBits.bounds;
    EventRecord ev;
    static const unsigned char *const lines[] = {
        "\pOrbit Duel 1.0",
        "\pA duel between two ships around a star, in the spirit of",
        "\pSpacewar! (1962). Play the computer, a friend on the same",
        "\pkeyboard, or another Mac over AppleTalk or a serial cable.",
        "\p",
        "\pA D turn, W thrust, Space or Shift fire, S hyperspace",
        "\p(each jump is riskier). Return pauses, M sound, Esc leaves.",
        "\pFirst to 5 wins.",
        "\p",
        "\pOriginal code, art and sound. MIT licence.",
        "\pClick or press a key to continue.",
    };
    short i, n = sizeof lines / sizeof lines[0], cx = (b.left + b.right) / 2;
    cursor_show();
    SetRect(&r, cx - 200, 60, cx + 200, 60 + 26 + n * 14);
    w = NewWindow(NULL, &r, "\p", true, dBoxProc, (WindowPtr)-1, false, 0);
    SetPort(w);
    for (i = 0; i < n; i++) {
        TextFont(i == 0 ? 0 : 3);
        TextSize(i == 0 ? 0 : 9);
        MoveTo(16, 22 + i * 14);
        DrawString(lines[i]);
    }
    dbg.about++;
    for (;;) {
        SystemTask();
        if (GetNextEvent(mDownMask | keyDownMask, &ev))
            break;
        if (dbg.cmd == CMD_CLOSE) {
            dbg.cmd = 0;
            break;
        }
    }
    DisposeWindow(w);
    SetPort(win);
    ui_dirty = 1;
}

/* ---- pacing ------------------------------------------------------------------------------ */
/* Sim tick k is due at t0 + 2k (30 steps per second of Ticks, 60.15 Hz on the Plus and SE).
   Returns how many steps are due now (at most 3: a slow frame catches up, a long stall is
   forgotten rather than raced through). */
short steps_due(long tick)
{
    long target = (long)(TICKS - t0) / 2;
    long due = target - tick + 1;
    if (due > 6) {
        t0 = TICKS - (unsigned long)tick * 2;
        due = 1;
    }
    return due < 0 ? 0 : due > 3 ? 3 : (short)due;
}

void pace_reset(long tick) { t0 = TICKS - (unsigned long)tick * 2; }

/* ---- HUD and messages (drawn into bg when they change) ---------------------------------- */
static const char *ship_name[2] = {"NEEDLE", "WEDGE"};
static short hud_score[2] = {-1, -1}, hud_msg = -1, hud_note = -1;

static void num(char *d, short v)
{
    if (v >= 10) *d++ = (char)('0' + v / 10);
    *d++ = (char)('0' + v % 10);
    *d = 0;
}

static void hud_scores(void)
{
    char s[24];
    short w;
    if (sim.score[0] != hud_score[0]) {
        hud_score[0] = sim.score[0];
        r_bg_clear(0, 0, 150, 34);
        strcpy(s, ship_name[0]);
        strcat(s, " ");
        num(s + strlen(s), sim.score[0]);
        r_text(s, 8, 6, 2, 0);
        if (kind == K_NET || kind == K_SOLO) {
            r_qd_begin();
            MoveTo(8, 32);
            DrawString((kind == K_NET ? local_ship == 0 : 1) ? "\pyou" : (kind == K_SOLO ? "\pcomputer" : "\pother Mac"));
            r_qd_end();
        }
        r_bg_dirty(0, 0, 150, 34);
    }
    if (sim.score[1] != hud_score[1]) {
        hud_score[1] = sim.score[1];
        r_bg_clear(ARENA_W - 150, 0, ARENA_W, 34);
        num(s, sim.score[1]);
        strcat(s, " ");
        strcat(s, ship_name[1]);
        w = r_text_w(s, 2);
        r_text(s, ARENA_W - 8 - w, 6, 2, 0);
        if (kind == K_NET || kind == K_SOLO) {
            const unsigned char *t = (kind == K_NET ? local_ship == 1 : 0) ? "\pyou" : (kind == K_SOLO ? "\pcomputer" : "\pother Mac");
            r_qd_begin();
            MoveTo(ARENA_W - 8 - StringWidth(t), 32);
            DrawString(t);
            r_qd_end();
        }
        r_bg_dirty(ARENA_W - 150, 0, ARENA_W, 34);
    }
}

#define MSG_Y0 236
#define MSG_Y1 292

static void msg_band(const char *big, const unsigned char *small)
{
    r_bg_clear(0, MSG_Y0, ARENA_W, MSG_Y1);
    if (big)
        r_text_c(big, MSG_Y0 + 8, 3, 0);
    if (small) {
        r_qd_begin();
        MoveTo((ARENA_W - StringWidth(small)) / 2, MSG_Y0 + 46);
        DrawString(small);
        r_qd_end();
    }
    r_bg_dirty(0, MSG_Y0, ARENA_W, MSG_Y1);
}

static void hud_message(void)
{
    short m;
    if (sim.paused) m = 1;
    else if (sim.phase == PH_MATCH_END) m = 10 + sim.winner;
    else if (sim.phase == PH_READY) m = sim.round == 1 ? 2 : 20 + sim.winner;
    else m = 0;
    if (kind == K_ATTRACT)
        m = 0;
    if (m == hud_msg)
        return;
    hud_msg = m;
    if (m == 0) {
        r_bg_clear(0, MSG_Y0, ARENA_W, MSG_Y1);
        r_bg_dirty(0, MSG_Y0, ARENA_W, MSG_Y1);
    } else if (m == 1)
        msg_band("PAUSED", "\pReturn to go on. Esc leaves the game.");
    else if (m == 2)
        msg_band("GET READY", kind == K_HOTSEAT ? "\pNeedle: W A S D + Space.  Wedge: I J K L + Shift." : 0);
    else if (m >= 20) {
        char s[24];
        if (m - 20 == 2)
            strcpy(s, "NOBODY SCORES");
        else {
            strcpy(s, ship_name[m - 20]);
            strcat(s, " SCORES");
        }
        msg_band(s, 0);
    } else {
        char s[24];
        short w = m - 10;
        if (kind == K_NET || kind == K_SOLO)
            strcpy(s, w == (kind == K_NET ? local_ship : 0) ? "YOU WIN!" : "YOU LOSE");
        else {
            strcpy(s, ship_name[w]);
            strcat(s, " WINS!");
        }
        msg_band(s, kind == K_NET ? "\pSpace or Return: rematch.  Esc: leave."
                                  : "\pSpace or Return: play again.  Esc: title.");
    }
}

/* a line of text in the link status band (network games) */
void hud_note_set(short id, const unsigned char *text)
{
    if (id == hud_note)
        return;
    hud_note = id;
    r_bg_clear(0, ARENA_H - 22, ARENA_W, ARENA_H);
    if (text) {
        r_qd_begin();
        MoveTo((ARENA_W - StringWidth(text)) / 2, ARENA_H - 8);
        DrawString(text);
        r_qd_end();
    }
    r_bg_dirty(0, ARENA_H - 22, ARENA_W, ARENA_H);
}

static void hud_reset(void)
{
    hud_score[0] = hud_score[1] = hud_msg = hud_note = -1;
}

/* ---- the game screen ---------------------------------------------------------------------- */
static void sound_for(short ev)
{
    if (ev & EV_MATCH) sound_event(SND_MATCH);
    else if (ev & EV_ROUND) sound_event(SND_ROUND);
    else if (ev & EV_BOOM) sound_event(SND_BOOM);
    else if (ev & EV_HYPER) sound_event(SND_HYPER);
    else if (ev & EV_HYPEROUT) sound_event(SND_BACK);
    else if (ev & (EV_FIRE0 | EV_FIRE1)) sound_event(SND_FIRE);
}

static void game_draw(void)
{
    hud_scores();
    hud_message();
    r_begin();
    r_world(&sim, gframe);
    r_end();
    gframe++;
}

static void start_game(short k, unsigned long seed, short ship)
{
    kind = k;
    local_ship = ship;
    sim_init(&sim, seed, 5);
    ai_init(&ai[0], seed * 3 + 11, ai_level, k == K_ATTRACT);
    ai_init(&ai[1], seed * 7 + 5, ai_level, k == K_ATTRACT);
    if (k == K_ATTRACT)
        return;
    screen = SCR_GAME;
    leaving = 0;
    cursor_hide();
    r_set_top_limit(0);
    r_stars(seed ^ 0x5EED);
    hud_reset();
    r_full();
    pace_reset(0);
    gframe = 0;
    dbg.kind = k;
    dbg.p0_shots = dbg.p0_jumps = 0;
    dbg.frames = 0;
    dbg.frame_ticks = 0;
    dbg.games++;
    FlushEvents(everyEvent, 0);
}

/* used by link.c: start a network game (both Macs call it with the same seed) */
void start_net_game(unsigned long seed, short ship)
{
    start_game(K_NET, seed, ship);
}

Sim *game_sim(void) { return &sim; }

static void game_frame(void)
{
    short n, i, ev = 0;
    unsigned long f0 = TICKS;
    keys_poll();
    if ((gframe & 3) == 0)
        poll_events(NULL);
    if (PRESSED(K_M))
        set_sound(sound_muted());
    if (ESC_PRESSED || dbg.cmd == CMD_TITLE) {
        dbg.cmd = 0;
        if (kind == K_NET)
            link_leave();
        title_enter();
        return;
    }
    if (kind == K_NET) {
        n = link_game_steps(&ev);            /* steps the sim as far as both Macs' inputs allow */
    } else {
        n = steps_due(sim.tick);
        for (i = 0; i < n; i++) {
            unsigned char in0, in1;
            if (kind == K_HOTSEAT)
                keys_hotseat(&in0, &in1);
            else {
                in0 = dbg.autopilot == 1 ? ai_input(&ai[0], &sim, 0) : keys_single();
                in1 = dbg.autopilot == 2 ? 0 : ai_input(&ai[1], &sim, 1);
            }
            sim_step(&sim, in0, in1);
            ev |= sim.events;
            dbg.in0 = in0;
        }
    }
    if (screen != SCR_GAME)
        return;                             /* the link ended the game */
    sound_for(ev);
    sound_engine(kind != K_HOTSEAT && sim.ship[kind == K_NET ? local_ship : 0].thrust && !sim.paused);
    sound_tick();
    if (n > 0) {
        game_draw();
        dbg.frames++;
        dbg.frame_ticks += (long)(TICKS - f0);
        last_draw = TICKS;
    } else {
        link_idle();
        if (TICKS - last_draw >= 6) {      /* waiting for the other Mac: keep the star and notes alive */
            game_draw();
            last_draw = TICKS;
        }
    }
    dbg.tick = sim.tick;
    dbg.p0_angle = sim.ship[0].angle;
    dbg.p0_thrust = sim.ship[0].thrust;
    dbg.p0_hyper = sim.ship[0].hyper;
    dbg.p1_angle = sim.ship[1].angle;
    {
        short k, nt = 0;
        for (k = 0; k < MAX_TORPS; k++)
            nt += sim.torp[0][k].life > 0;
        dbg.p0_torps = nt;
    }
    if (ev & EV_FIRE0) dbg.p0_shots++;
    if (ev & EV_HYPER) dbg.p0_jumps++;
    dbg.score0 = sim.score[0];
    dbg.score1 = sim.score[1];
    dbg.phase = sim.phase;
    dbg.matches = sim.matches;
    dbg.round = sim.round;
}

/* ---- the title screen ---------------------------------------------------------------------- */
static void title_draw_bg(void)
{
    static const unsigned char *const lines[] = {
        "\p1     Play the computer",
        "\p2     Two players on this keyboard",
        "\p3     Network game over AppleTalk",
        "\p4     Serial cable game (modem ports)",
    };
    static const unsigned char *const lv[3] = {"\pEasy", "\pNormal", "\pHard"};
    short i, x;
    r_stars(0x0D0E1);
    r_bg_black(40, 22, ARENA_W - 40, 92);
    r_text_c("ORBIT DUEL", 30, 6, 1);
    r_bg_black(60, 218, ARENA_W - 60, ARENA_H);
    r_qd_begin();
    x = 112;
    for (i = 0; i < 4; i++) {
        MoveTo(x, 236 + i * 17);
        DrawString(lines[i]);
    }
    MoveTo(x, 236 + 4 * 17 + 4);
    DrawString("\pTab   Computer pilot: ");
    DrawString(lv[ai_level]);
    TextFont(3);
    TextSize(9);
    MoveTo((ARENA_W - StringWidth("\pA D turn   W thrust   Space or Shift fire   S hyperspace   Esc quits")) / 2, 328);
    DrawString("\pA D turn   W thrust   Space or Shift fire   S hyperspace   Esc quits");
    r_qd_end();
}

static void title_enter(void)
{
    screen = SCR_TITLE;
    link_close();
    sound_stop();
    start_game(K_ATTRACT, 0xA77AC7 + TICKS, 0);
    r_set_top_limit(mbar_h());
    title_draw_bg();
    cursor_hide();
    DrawMenuBar();
    r_full();
    cursor_show();
    pace_reset(0);
    hud_reset();
    dbg.screen = SCR_TITLE;
    FlushEvents(everyEvent, 0);
}

static void title_frame(void)
{
    short key, n, i;
    Point click = {0, 0};
    keys_poll();
    key = poll_events(&click);
    if (screen != SCR_TITLE)
        return;                             /* a menu command started something */
    if (ui_dirty) {
        ui_dirty = 0;
        if (front_is_ours()) {
            title_draw_bg();
            full_redraw();
        }
    }
    if (dbg.cmd) {
        short c = (short)dbg.cmd;
        dbg.cmd = 0;
        if (c == CMD_SOLO) { if (dbg.arg >= 0 && dbg.arg <= 2) set_level((short)dbg.arg); start_game(K_SOLO, TICKS, 0); return; }
        if (c == CMD_HOTSEAT) { start_game(K_HOTSEAT, TICKS, 0); return; }
        if (c == CMD_ATALK) { link_screen(LINK_ATALK); return; }
        if (c == CMD_SERIAL) { link_screen(LINK_SERIAL); return; }
        if (c == CMD_ABOUT) { about(); return; }
        if (c == CMD_QUIT) quit();
    }
    if (key == '1') { start_game(K_SOLO, TICKS, 0); return; }
    if (key == '2') { start_game(K_HOTSEAT, TICKS, 0); return; }
    if (key == '3') { link_screen(LINK_ATALK); return; }
    if (key == '4') { link_screen(LINK_SERIAL); return; }
    if (key == '\t') {
        set_level((ai_level + 1) % 3);
        ui_dirty = 1;
    }
    if (key == 'm' || key == 'M')
        set_sound(sound_muted());
    if (key == 0x1B || key == '`')
        quit();
    dbg.front_ok = front_is_ours();
    if (!dbg.front_ok)
        return;                             /* a desk accessory is in front: leave it alone */
    n = steps_due(sim.tick);
    for (i = 0; i < n; i++)
        sim_step(&sim, ai_input(&ai[0], &sim, 0), ai_input(&ai[1], &sim, 1));
    if (n > 0) {
        Rect sr;
        SetRect(&sr, r_view_left, r_view_top, r_view_left + ARENA_W, r_view_top + ARENA_H);
        ShieldCursor(&sr, (Point){0, 0});
        r_begin();
        r_world(&sim, gframe);
        r_end();
        ShowCursor();
        gframe++;
        dbg.title_frames++;
    }
    sound_tick();
}

/* ---- main ---------------------------------------------------------------------------------- */
int main(void)
{
    Rect r;

    dbg.magic = DBG_MAGIC;
    dbg.version = 1;
    dbg.notmagic = ~DBG_MAGIC;

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
    set_level(1);
    r = qd.screenBits.bounds;
    r.top += mbar_h();
    win = NewWindow(NULL, &r, "\p", true, plainDBox, (WindowPtr)-1, false, 0);
    SetPort(win);
    r_init();
    sound_init();
    set_sound(1);
    link_init();
    dbg.bounds = qd.screenBits.bounds;
    dbg.view_left = r_view_left;
    dbg.view_top = r_view_top;
    dbg.freemem = FreeMem();
    title_enter();
    for (;;) {
        if (screen == SCR_TITLE)
            title_frame();
        else if (screen == SCR_GAME)
            game_frame();
        else
            link_frame();
        dbg.screen = screen;
    }
    return 0;
}

/* used by link.c */
void set_screen_link(void)
{
    screen = SCR_LINK;
    dbg.screen = SCR_LINK;
    sound_stop();
}
void back_to_title(void) { title_enter(); }
short ui_take_dirty(void) { short d = ui_dirty; ui_dirty = 0; return d; }

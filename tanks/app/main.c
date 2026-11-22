/* Tank Duel for the Macintosh Plus: menus, title and attract demo, stages, input, AI, effects.
 *
 * The game runs the deterministic simulation (../sim/sim.c) at 30 ticks per second (one tick per
 * two Mac ticks); AI tanks decide at 10 Hz (tick % 3 == index % 3) with a ternary neural policy
 * compiled to 68000 code (policies.s, from gen_policy.py) or a scripted bot. Rendering draws into
 * an offscreen buffer and copies only what changed (render.c), as often as the sim allows.
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
#include <Files.h>
#include <string.h>
#include "sim.h"
#include "render.h"
#include "sound.h"
#include "art.h"
#include "policies.h"

#define TICKS (*(volatile unsigned long *)(long)ticks_addr)
static volatile long ticks_addr = 0x16A;
#define MAGIC 0x544E4B44UL           /* 'TNKD' */

/* ------------------------------------------------------------------ debug block (Lua reads it) */
struct Dbg {
    unsigned long magic, version, notmagic;   /*   0 */
    long state;                               /*  12: 0 init 1 title 2 play 3 pause 4 over 5 stage banner */
    long frames;                              /*  16: frames drawn */
    long simticks;                            /*  20: sim ticks run */
    long fps100;                              /*  24: frames per second x 100 (last 2 s window) */
    long tps100;                              /*  28: sim ticks per second x 100 */
    long stage, score, lives;                 /*  32, 36, 40 */
    long freemem;                             /*  44 */
    long nhash;                               /*  48: attract demo hashes recorded */
    unsigned long hash[32];                   /*  52: hash after demo ticks 30, 60, ... */
    short obs[40];                            /* 180: last observation of demo tank 1 */
    long logits[15];                          /* 260: its logits */
    long probe_tick;                          /* 320: the tick of that decision */
    long ndecide;                             /* 324: AI decisions made */
    long cmd;                                 /* 328: written by Lua: 1 start, 2 AI view, 3 bench, 4 autoplay */
    long bench[4];                            /* 332: ticks for 300 obs+policy, 300 sim ticks, 300 frames drawn, 0 */
    long autoplay;                            /* 348: player driven by the hunter bot (tests) */
    long ai_view, difficulty, sound;          /* 352, 356, 360 */
    long win_frames, win_ticks;               /* 364, 368 */
    long drops;                               /* 372: sim ticks dropped (frame took > 3 ticks) */
    long probe_pol;                           /* 376 */
    long demo_seed;                           /* 380 */
    long start_stage;                         /* 384: written by Lua: the stage a test game starts at */
    long bench2[6];                           /* 388: ticks for 300 x: obs, policy, 8 rays, los, aim, target */
} dbg;

/* ------------------------------------------------------------------ state */
enum { C_HUMAN, C_BOT_SITTER, C_BOT_HUNTER, C_BOT_WANDER, C_POLICY };
typedef struct { short kind, pol; } TCtl;

static Sim S;
static TCtl ctl[MAXT];
static signed char last_act[MAXT][4];

/* what each AI tank sensed at its last decision ("Show the AI" draws this, so it costs nothing per frame
   but the plotting): its position, hull, sensor ray lengths and predicted shell path (pixels) */
typedef struct {
    short valid, x, y, np, hit;
    unsigned char hull, nray[NRAY];
    short px[AIMN + 2], py[AIMN + 2];
} Seen;
static Seen seen[MAXT];
static short ai_view;
static void remember(short i, const short *obs)
{
    Seen *v = &seen[i];
    Tank *t = &S.t[i];
    short k, j, pts[2 * (AIMN + 2)], self;
    v->valid = 1;
    v->x = t->x;
    v->y = t->y;
    v->hull = t->hull;
    for (k = 0; k < NRAY; k++)
        v->nray[k] = (unsigned char)(obs ? obs[k] / 6 : sim_ray(&S, t->x, t->y, (unsigned char)(t->hull + 32 * k)));
    v->np = 0;
    v->hit = 0;
    j = sim_target(&S, i);
    if (j >= 0) {
        v->np = sim_aim(&S, i, j, &v->hit, &self, pts);
        for (k = 0; k < v->np; k++) {
            v->px[k] = pts[2 * k] >> 4;
            v->py[k] = pts[2 * k + 1] >> 4;
        }
    }
}
static WindowPtr win;
static short difficulty = 1, sound_want = 1;
static long score, hiscore;
static short stage, lives;
static short scratch[128];
static short probe_on;               /* record tank 1's decisions (first attract demo round) */
static long logits[15];
static const PolFn pol_fn[NPOL] = POLICY_TABLE;
static const short pol_shift[NPOL] = POLICY_SHIFT;

/* policy for (type, difficulty): an index into the policy table, or -1 for a scripted bot */
#include "polmap.h"

/* ------------------------------------------------------------------ input */
static unsigned char km[16], km_prev[16];
#define KEY(c) ((km[(c) >> 3] >> ((c) & 7)) & 1)
#define WAS(c) ((km_prev[(c) >> 3] >> ((c) & 7)) & 1)
#define PRESSED(c) (KEY(c) && !WAS(c))
#define K_A 0x00
#define K_S 0x01
#define K_D 0x02
#define K_W 0x0D
#define K_M 0x2E
#define K_P 0x23
#define K_N 0x2D
#define K_Q 0x0C
#define K_SPACE 0x31
#define K_RETURN 0x24
#define K_ENTER 0x4C
#define K_TAB 0x30
#define K_SHIFT 0x38
#define K_CMD 0x37
#define ESC_PRESSED (PRESSED(0x35) || PRESSED(0x32))
#define CMD_DOWN (KEY(K_CMD) || KEY(0x3B))

static void poll_keys(void)
{
    memcpy(km_prev, km, 16);
    GetKeys(km);
}

static Point mouse_pt(void) { return *(Point *)0x830; }
static short mouse_down(void) { return !(*(volatile unsigned char *)0x172 & 0x80); }

/* ------------------------------------------------------------------ effects */
#define MAXFX 20
typedef struct { short kind, x, y, t; } Fx;    /* kind: 1 boom, 2 spark, 3 puff */
static Fx fx[MAXFX];

static void fx_add(short kind, short x, short y)
{
    short k;
    for (k = 0; k < MAXFX; k++)
        if (!fx[k].kind) {
            fx[k].kind = kind;
            fx[k].x = x;
            fx[k].y = y;
            fx[k].t = 0;
            return;
        }
}

static void fx_step(void)       /* once per sim tick */
{
    short k;
    for (k = 0; k < MAXFX; k++) {
        Fx *f = &fx[k];
        if (!f->kind)
            continue;
        f->t++;
        if ((f->kind == 1 && f->t >= 18) || (f->kind == 2 && f->t >= 4) || (f->kind == 3 && f->t >= 4))
            f->kind = 0;
    }
}

/* ------------------------------------------------------------------ AI */
static void ai_decide(short i)
{
    TCtl c = ctl[i];
    Tank *t = &S.t[i];
    dbg.ndecide++;
    if (c.kind == C_POLICY) {
        short obs[NOBS], act[4];
        sim_obs(&S, i, obs);
        pol_fn[c.pol](obs, scratch, logits);
        sim_pick(&S, (const int32_t *)logits, pol_shift[c.pol], act);    /* sampled at temperature 0.5 */
        sim_apply_action(&S, i, act);
        if (ai_view)
            remember(i, obs);
        if (probe_on && i == 1) {
            memcpy(dbg.obs, obs, sizeof obs);
            memcpy(dbg.logits, logits, sizeof logits);
            dbg.probe_tick = S.tick;
            dbg.probe_pol = c.pol;
        }
    } else if (c.kind >= C_BOT_SITTER && c.kind <= C_BOT_WANDER) {
        bot_act(&S, i, c.kind == C_BOT_SITTER ? BOT_SITTER : c.kind == C_BOT_HUNTER ? BOT_HUNTER : BOT_WANDER);
        if (ai_view)
            remember(i, 0);
    }
    last_act[i][0] = t->drive;
    last_act[i][1] = t->turn;
    last_act[i][2] = (signed char)(t->tgt - t->tur);
    last_act[i][3] = t->fire;
}

static TCtl ctl_for(short type)
{
    TCtl c;
    short p = POLMAP[type][difficulty];
    if (p >= 0) {
        c.kind = C_POLICY;
        c.pol = p;
    } else {
        c.kind = type == T_SNIPER ? C_BOT_SITTER : type == T_CAREFUL ? C_BOT_WANDER : C_BOT_HUNTER;
        c.pol = 0;
    }
    return c;
}

/* ------------------------------------------------------------------ player input -> tank 0 */
static short mouse_valid;

static void player_input(void)
{
    Tank *t = &S.t[0];
    short fire;
    if (dbg.autoplay) {
        bot_act(&S, 0, BOT_HUNTER);
        return;
    }
    t->drive = (KEY(K_W) || KEY(0x4D) || KEY(0x7E)) ? 1 : (KEY(K_S) || KEY(0x48) || KEY(0x7D)) ? -1 : 0;
    t->turn = (KEY(K_A) || KEY(0x46) || KEY(0x7B)) ? -1 : (KEY(K_D) || KEY(0x42) || KEY(0x7C)) ? 1 : 0;
    {
        Point m = mouse_pt();
        short mx = m.h - r_arena_rect.left, my = m.v - r_arena_rect.top;
        short dx = mx - (t->x >> 4), dy = my - (t->y >> 4);
        mouse_valid = m.v >= r_arena_rect.top - 2;
        if (dx || dy)
            t->tgt = sim_atan2(dy, dx);
        fire = KEY(K_SPACE) || KEY(K_SHIFT) || (mouse_down() && m.v >= *(short *)0xBAA);
    }
    t->fire = (signed char)fire;
}

/* ------------------------------------------------------------------ drawing */
static const short DESIGN[4] = {0, 1, 2, 3};

/* "Show the AI" is drawn straight onto the screen with XOR after each frame is presented, and
   recorded, so that XOR-ing the same pixels again before the next frame leaves the screen exactly
   as the frame buffer had it: no full-frame copies, and the dots show on walls too. */
#define MAXOV 1400
static unsigned short ov_off[MAXOV];
static unsigned char ov_m[MAXOV];
static short n_ov;

static unsigned short ov_row[AH];     /* screen offset of each arena row */

static inline __attribute__((always_inline)) void ov_px(short x, short y)
{
    unsigned short off;
    unsigned char m;
    if ((unsigned short)x >= AW || (unsigned short)y >= AH || n_ov >= MAXOV)
        return;
    off = ov_row[y] + (x >> 3);
    m = (unsigned char)(0x80 >> (x & 7));
    r_screen[off] ^= m;
    ov_off[n_ov] = off;
    ov_m[n_ov++] = m;
}

/* a pixel known to be inside the arena (sensor samples, path points) */
static const unsigned char ov_bit[8] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};
static inline __attribute__((always_inline)) void ov_in(unsigned short x, unsigned short y)
{
    unsigned short o = ov_row[y] + (x >> 3);
    unsigned char m = ov_bit[x & 7];
    short n = n_ov;
    if (n < MAXOV) {
        r_screen[o] ^= m;
        ov_off[n] = o;
        ov_m[n] = m;
        n_ov = n + 1;
    }
}
#define OV_IN(x, y) ov_in((unsigned short)(x), (unsigned short)(y))

static void ov_erase(void)
{
    short k;
    for (k = 0; k < n_ov; k++)
        r_screen[ov_off[k]] ^= ov_m[k];
    n_ov = 0;
}

static void ov_dot(short x, short y)
{
    ov_px(x, y);
    ov_px(x + 1, y);
    ov_px(x, y + 1);
    ov_px(x + 1, y + 1);
}

static void ov_line(short x0, short y0, short x1, short y1)
{
    short dx = x1 - x0, dy = y1 - y0, sx = 1, sy = 1, err;
    if (dx < 0) { dx = -dx; sx = -1; }
    if (dy < 0) { dy = -dy; sy = -1; }
    err = dx - dy;
    for (;;) {
        ov_px(x0, y0);
        if (x0 == x1 && y0 == y1)
            break;
        {
            short e2 = err << 1;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 < dx) { err += dx; y0 += sy; }
        }
    }
}

/* what the tank sensed at its last decision: every other sample point of its 8 sensor rays (a
   cross where one met a wall), its predicted shell path (big dots when it reaches the target), and
   its intended move (an arrow along the hull, a dot on the side it turns to) */
static short ov_parts = 7;             /* (benchmark) 1 rays, 2 path, 4 move */
static void draw_ai_view(short i)
{
    Tank *t = &S.t[i];
    Seen *v = &seen[i];
    short cx = t->x >> 4, cy = t->y >> 4, k, n;
    unsigned char h = t->hull;
    if (v->valid && (ov_parts & 1)) {
        for (k = 0; k < NRAY; k++) {
            unsigned char a = (unsigned char)(v->hull + 32 * k);
            short dx = (short)(((long)RAYSTEP * COSQ(a) + 8192) >> 14), dy = (short)(((long)RAYSTEP * SINQ(a) + 8192) >> 14);
            short x = v->x, y = v->y, nr = v->nray[k], dx4 = dx << 2, dy4 = dy << 2;
            for (n = 4; n < nr; n += 4) {          /* every 4th sample point (32 px apart) */
                x += dx4;
                y += dy4;
                OV_IN(x >> 4, y >> 4);
            }
            if (nr <= RAYN) {                       /* where the ray met a wall: a short diagonal */
                short ex = (short)((v->x + (long)dx * nr) >> 4), ey = (short)((v->y + (long)dy * nr) >> 4);
                ov_px(ex - 1, ey - 1);
                ov_px(ex, ey);
                ov_px(ex + 1, ey + 1);
            }
        }
    }
    if (v->valid && (ov_parts & 2))
        for (k = 1; k < v->np; k++)
            if (v->hit || !(k & 3))           /* reaches the target: every point, else every 4th */
                OV_IN(v->px[k], v->py[k]);
    if (!(ov_parts & 4))
        return;
    if (last_act[i][0]) {               /* intended move: a line ahead of (or behind) the hull */
        short c = COSQ(h), sn = SINQ(h), d = last_act[i][0];
        short ex = cx + (short)(((long)(20 * d) * c) >> 14), ey = cy + (short)(((long)(20 * d) * sn) >> 14);
        short bx = cx + (short)(((long)(11 * d) * c) >> 14), by = cy + (short)(((long)(11 * d) * sn) >> 14);
        ov_line(bx, by, ex, ey);
    }
    if (last_act[i][1]) {
        unsigned char sd = (unsigned char)(h + (last_act[i][1] > 0 ? 64 : -64));
        ov_dot(cx + (short)(((long)12 * COSQ(sd)) >> 14), cy + (short)(((long)12 * SINQ(sd)) >> 14));
    }
}

static void ai_overlay(short show_ai)
{
    short i;
    if (!show_ai)
        return;
    for (i = 0; i < S.nt; i++)
        if (S.t[i].alive && ctl[i].kind != C_HUMAN)
            draw_ai_view(i);
}

static short frame_no;

static void draw_world(short show_ai, short cross)
{
    short i, k;
    ov_erase();
    r_begin(0);
    for (i = 0; i < S.nt; i++) {
        Tank *t = &S.t[i];
        short x = t->x >> 4, y = t->y >> 4, ph;
        if (!t->alive)
            continue;
        r_obj(i);
        if (t->shield > 0 && (frame_no & 2))
            r_spr(SPR_RING, x, y);
        ph = (((t->x + t->y) >> 6) ^ (t->hull >> 3)) & 1;
        r_spr(SPR_HULL_AT(DESIGN[t->type], ph, ((t->hull + 4) >> 3) & 31), x, y);
        r_spr(SPR_TUR_AT(DESIGN[t->type], ((t->tur + 4) >> 3) & 31), x, y);
    }
    for (k = 0; k < MAXS; k++)
        if (S.s[k].alive) {
            r_obj(OBJ_SHELL + k);
            r_spr(SPR_SHELL, S.s[k].x >> 4, S.s[k].y >> 4);
        }
    for (k = 0; k < MAXFX; k++) {
        Fx *f = &fx[k];
        r_obj(OBJ_FX + k);
        if (f->kind == 1)
            r_spr(SPR_BOOM + (f->t / 3 < 6 ? f->t / 3 : 5), f->x, f->y);
        else if (f->kind == 2)
            r_spr(SPR_SPARK + (f->t >> 1), f->x, f->y);
        else if (f->kind == 3)
            r_spr(SPR_SPARK + 1, f->x, f->y);
    }
    if (cross) {
        Point m = mouse_pt();
        short mx = m.h - r_arena_rect.left, my = m.v - r_arena_rect.top;
        r_obj(OBJ_CROSS);
        if (mx >= 6 && mx < AW - 6 && my >= 6 && my < AH - 6)
            r_spr(SPR_CROSS, mx, my);
    }
    frame_no++;
}

static void fb_text(short x, short y, const unsigned char *s, short centre)
{
    GrafPtr old;
    GetPort(&old);
    SetPort(&r_port);
    TextFont(0);
    TextSize(12);
    TextMode(srcCopy);               /* white behind the letters: readable over walls */
    if (centre)
        x -= StringWidth(s) / 2;
    MoveTo(x, y);
    DrawString(s);
    SetPort(old);
}

static void banner(const unsigned char *a, const unsigned char *b)
{
    short w = 220, h = b ? 50 : 32;
    r_obj(OBJ_BANNER);
    r_box(AW / 2 - w / 2, AH / 2 - h / 2, AW / 2 + w / 2, AH / 2 + h / 2);
    fb_text(AW / 2, AH / 2 - h / 2 + 20, a, 1);
    if (b)
        fb_text(AW / 2, AH / 2 - h / 2 + 38, b, 1);
}

/* ------------------------------------------------------------------ HUD (straight onto the screen) */
static long hud_key = -1;

static void pstr_num(unsigned char *d, const char *pre, long v)
{
    unsigned char n[16];
    short k = 0, i;
    while (pre[k]) {
        d[1 + k] = (unsigned char)pre[k];
        k++;
    }
    NumToString(v, n);
    for (i = 1; i <= n[0]; i++)
        d[1 + k++] = n[i];
    d[0] = (unsigned char)k;
}

static void draw_hud(short force)
{
    long key = score * 131 + stage * 7 + lives * 1009 + dbg.state * 31;
    short i, enemies = 0;
    unsigned char s[40];
    Rect r = r_hud_rect;
    for (i = 1; i < S.nt; i++)
        enemies += S.t[i].alive;
    key = key * 3 + enemies;
    if (!force && key == hud_key)
        return;
    hud_key = key;
    SetPort(win);
    {
        Rect lr = r;
        GlobalToLocal((Point *)&lr.top);
        GlobalToLocal((Point *)&lr.bottom);
        EraseRect(&lr);
        TextFont(0);
        TextSize(12);
        TextMode(srcOr);
        if (dbg.state == 1) {
            pstr_num(s, "HIGH SCORE ", hiscore);
            MoveTo(lr.left + 4, lr.top + 13);
            DrawString(s);
            return;
        }
        pstr_num(s, "STAGE ", stage + 1);
        MoveTo(lr.left + 4, lr.top + 13);
        DrawString(s);
        pstr_num(s, "SCORE ", score);
        MoveTo(lr.left + 120, lr.top + 13);
        DrawString(s);
        pstr_num(s, "LIVES ", lives);
        MoveTo(lr.left + 230, lr.top + 13);
        DrawString(s);
        pstr_num(s, "FOES ", enemies);
        MoveTo(lr.left + 310, lr.top + 13);
        DrawString(s);
    }
}

/* ------------------------------------------------------------------ settings and high score file */
static const unsigned char prefs_name[] = "\pTank Duel Prefs";
static short prefs_dirty;
typedef struct { long magic, hiscore; short sound, ai_view, difficulty, pad; } Prefs;

static void load_prefs(void)
{
    short ref;
    long n = sizeof(Prefs);
    Prefs p;
    if (FSOpen(prefs_name, 0, &ref))
        return;
    if (!FSRead(ref, &n, &p) && n == sizeof(Prefs) && p.magic == MAGIC) {
        hiscore = p.hiscore;
        sound_want = p.sound != 0;
        ai_view = p.ai_view != 0;
        difficulty = p.difficulty >= 0 && p.difficulty <= 2 ? p.difficulty : 1;
    }
    FSClose(ref);
}

static void save_prefs(void)
{
    short ref;
    long n = sizeof(Prefs);
    Prefs p;
    if (!prefs_dirty)
        return;
    prefs_dirty = 0;
    p.magic = MAGIC;
    p.hiscore = hiscore;
    p.sound = sound_want;
    p.ai_view = ai_view;
    p.difficulty = difficulty;
    p.pad = 0;
    sound_stop();
    Create(prefs_name, 0, 'TnkD', 'TDpf');
    if (!FSOpen(prefs_name, 0, &ref)) {
        SetFPos(ref, fsFromStart, 0);
        FSWrite(ref, &n, &p);
        SetEOF(ref, n);
        FSClose(ref);
        FlushVol(0, 0);
    }
}

/* ------------------------------------------------------------------ menus */
enum { M_APPLE = 128, M_FILE, M_OPTIONS };
static MenuHandle opt_menu;
static short cursor_shown = 1;
static short want_new, want_quit, want_title;

static void save_prefs(void);
static void quit_app(void)
{
    save_prefs();
    sound_off();
    FlushEvents(everyEvent, 0);
    ShowCursor();
    ExitToShell();
}

static void redraw_all(void)
{
    SetPort(win);
    PaintRect(&win->portRect);
    hud_key = -1;
    r_present_all();
    n_ov = 0;                       /* the overlay went with it */
    draw_hud(1);
}

static void present(void)
{
    r_present();
    ai_overlay(ai_view);
}

static void about_box(void)
{
    static const unsigned char *const lines[] = {
        "\pTank Duel: outfight neural-network tanks.",
        "\pW/S drive, A/D turn, the mouse aims,",
        "\pclick (or Space / Shift) fires. Shells bounce once.",
        "\pReturn or P pauses, Tab shows the AI's sensors,",
        "\pM sound, Esc back to the title / quit.",
        "\pThe enemy tanks are small neural nets trained",
        "\pby reinforcement learning. Made in 2026.",
    };
    Rect r, sb = qd.screenBits.bounds;
    WindowPtr w;
    short i, cx = (short)((sb.left + sb.right) / 2), cy = (short)((sb.top + sb.bottom) / 2);
    SetRect(&r, cx - 170, cy - 72, cx + 170, cy + 72);
    w = NewWindow(NULL, &r, "\p", true, dBoxProc, (WindowPtr)-1, false, 0);
    if (!w)
        return;
    SetPort(w);
    TextFont(0);
    for (i = 0; i < 7; i++) {
        MoveTo(12, (short)(20 + i * 18));
        DrawString(lines[i]);
    }
    while (Button())
        ;
    for (;;) {
        poll_keys();
        if (Button() || PRESSED(K_RETURN) || PRESSED(K_SPACE) || ESC_PRESSED)
            break;
        SystemTask();
    }
    while (Button())
        ;
    DisposeWindow(w);
    FlushEvents(everyEvent, 0);
    redraw_all();
}

static void set_checks(void)
{
    short k;
    CheckItem(opt_menu, 1, sound_want);
    CheckItem(opt_menu, 2, ai_view);
    for (k = 0; k < 3; k++)
        CheckItem(opt_menu, 4 + k, difficulty == k);
}

static void set_sound(short on)
{
    if (on != sound_want)
        prefs_dirty = 1;
    sound_want = on;
    sound_set(on);
    dbg.sound = on;
    set_checks();
}

static void set_ai_view(short on)
{
    if (on != ai_view)
        prefs_dirty = 1;
    ai_view = on;
    dbg.ai_view = on;
    set_checks();
}

static void do_menu(long mr)
{
    short menu = (short)(mr >> 16), item = (short)mr;
    if (menu == M_APPLE && item == 1)
        about_box();
    else if (menu == M_FILE && item == 1)
        want_new = 1;
    else if (menu == M_FILE && item == 3)
        want_quit = 1;
    else if (menu == M_OPTIONS) {
        if (item == 1)
            set_sound(!sound_want);
        else if (item == 2)
            set_ai_view(!ai_view);
        else if (item >= 4 && item <= 6) {
            difficulty = item - 4;
            dbg.difficulty = difficulty;
            prefs_dirty = 1;
            set_checks();
        }
    }
    HiliteMenu(0);
}

static void setup_menus(void)
{
    MenuHandle m;
    m = NewMenu(M_APPLE, "\p\024");
    AppendMenu(m, "\pAbout Tank Duel\311");
    InsertMenu(m, 0);
    m = NewMenu(M_FILE, "\pFile");
    AppendMenu(m, "\pNew Game/N;(-;Quit/Q");
    InsertMenu(m, 0);
    opt_menu = NewMenu(M_OPTIONS, "\pOptions");
    AppendMenu(opt_menu, "\pSound;Show the AI;(-;Easy;Normal;Hard");
    InsertMenu(opt_menu, 0);
    set_checks();
    DrawMenuBar();
}

/* cursor hidden over the arena (we draw a crosshair), shown elsewhere so the menus work */
static void track_cursor(short in_game)
{
    Point pt = mouse_pt();
    short over = in_game && pt.h > r_arena_rect.left - 16 && pt.h < r_arena_rect.right + 16
                 && pt.v > r_hud_rect.top - 16 && pt.v < r_arena_rect.bottom + 16;
    if (over && cursor_shown) {
        HideCursor();
        cursor_shown = 0;
    } else if (!over && !cursor_shown) {
        ShowCursor();
        cursor_shown = 1;
    }
}

/* common per-frame housekeeping; returns 1 if the menu bar was used */
static short housekeeping(short in_game)
{
    track_cursor(in_game);
    if (PRESSED(K_M))
        set_sound(!sound_want);
    if (PRESSED(K_TAB))
        set_ai_view(!ai_view);
    if (CMD_DOWN && PRESSED(K_Q))
        want_quit = 1;
    if (CMD_DOWN && PRESSED(K_N))
        want_new = 1;
    if (dbg.cmd == 2) {
        dbg.cmd = 0;
        set_ai_view(!ai_view);
    }
    if (mouse_down()) {
        Point pt = mouse_pt();
        if (pt.v < *(short *)0xBAA) {
            long mr;
            if (!cursor_shown) {
                ShowCursor();
                cursor_shown = 1;
            }
            sound_stop();
            mr = MenuSelect(pt);
            do_menu(mr);
            FlushEvents(everyEvent, 0);
            return 1;
        }
    }
    if (!(frame_no & 31))
        SystemTask();
    if (want_quit)
        quit_app();
    return 0;
}

/* ------------------------------------------------------------------ timing */
static unsigned long next_sim, win_start;
static long win_frames0, win_ticks0;

static void timing_reset(void)
{
    next_sim = TICKS + 2;
}

/* how many sim ticks are due now (0..3); waits for the first one */
static short ticks_due(void)
{
    short n = 0;
    unsigned long now;
    for (;;) {
        now = TICKS;
        if ((long)(now - next_sim) >= 0)
            break;
        sound_tick();
    }
    while ((long)(now - next_sim) >= 0 && n < 3) {
        n++;
        next_sim += 2;
    }
    if ((long)(now - next_sim) >= 0) {        /* far behind: drop the rest */
        dbg.drops += (now - next_sim) / 2 + 1;
        next_sim = now + 2;
    }
    return n;
}

static void measure(void)
{
    unsigned long now = TICKS;
    dbg.frames++;
    if (now - win_start >= 120) {
        long f = dbg.frames - win_frames0, t = dbg.simticks - win_ticks0, dt = (long)(now - win_start);
        dbg.fps100 = f * 6015 / dt;
        dbg.tps100 = t * 6015 / dt;
        dbg.win_frames = f;
        dbg.win_ticks = dt;
        win_start = now;
        win_frames0 = dbg.frames;
        win_ticks0 = dbg.simticks;
    }
}

/* ------------------------------------------------------------------ events of a sim tick */
static void sfx(short in_game, short what)    /* the attract demo is silent */
{
    if (in_game)
        sound_play(what);
}

static void handle_events(short in_game)
{
    short k;
    for (k = 0; k < S.nev; k++) {
        SimEv *e = &S.ev[k];
        switch (e->kind) {
        case EV_FIRE:
            sfx(in_game, e->a == 0 ? SND_FIRE : SND_EFIRE);
            break;
        case EV_RICOCHET:
            fx_add(2, e->x >> 4, e->y >> 4);
            sfx(in_game, SND_RICOCHET);
            break;
        case EV_SHELL_END:
            fx_add(3, e->x >> 4, e->y >> 4);
            sfx(in_game, SND_PUFF);
            break;
        case EV_SHELLS:
            fx_add(2, e->x >> 4, e->y >> 4);
            sfx(in_game, SND_SHELLS);
            break;
        case EV_CLANG:
        case EV_SHIELD:
            fx_add(2, e->x >> 4, e->y >> 4);
            sfx(in_game, SND_CLANG);
            break;
        case EV_DUD:
            fx_add(3, e->x >> 4, e->y >> 4);
            sfx(in_game, SND_DUD);
            break;
        case EV_KILL:
            fx_add(1, e->x >> 4, e->y >> 4);
            sfx(in_game, SND_BOOM);
            if (in_game && e->a != 0) {
                static const short pts[4] = {0, 100, 150, 200};
                score += pts[S.t[e->a].type];
            }
            break;
        }
    }
}

static void sim_step(short in_game)
{
    short i;
    if (in_game && S.t[0].alive)
        player_input();
    for (i = 0; i < S.nt; i++)
        if (S.t[i].alive && ctl[i].kind != C_HUMAN && (S.tick % DECIDE) == (i % DECIDE))
            ai_decide(i);
    sim_tick(&S);
    dbg.simticks++;
    handle_events(in_game);
    fx_step();
}

/* ------------------------------------------------------------------ benchmark (Lua: cmd 3) */
static void run_bench(void)
{
    static Sim B;
    unsigned long t0;
    short k, i;
    memcpy(&B, &S, sizeof S);
    {
        short obs[NOBS], h, sf, j = sim_target(&S, 1);
        if (j < 0)
            j = 0;
        t0 = TICKS;
        for (k = 0; k < 300; k++)
            sim_obs(&S, 1, obs);
        dbg.bench2[0] = (long)(TICKS - t0);
        t0 = TICKS;
        for (k = 0; k < 300; k++)
            pol_fn[0](obs, scratch, logits);
        dbg.bench2[1] = (long)(TICKS - t0);
        t0 = TICKS;
        for (k = 0; k < 300; k++)
            for (i = 0; i < 8; i++)
                sim_ray(&S, S.t[1].x, S.t[1].y, (unsigned char)(S.t[1].hull + 32 * i));
        dbg.bench2[2] = (long)(TICKS - t0);
        t0 = TICKS;
        for (k = 0; k < 300; k++)
            sim_los(&S, S.t[1].x, S.t[1].y, S.t[j].x, S.t[j].y);
        dbg.bench2[3] = (long)(TICKS - t0);
        t0 = TICKS;
        for (k = 0; k < 300; k++)
            sim_aim(&S, 1, j, &h, &sf, 0);
        dbg.bench2[4] = (long)(TICKS - t0);
        t0 = TICKS;
        for (k = 0; k < 300; k++)
            sim_target(&S, 1);
        dbg.bench2[5] = (long)(TICKS - t0);
    }
    t0 = TICKS;
    for (k = 0; k < 300; k++) {
        short obs[NOBS];
        sim_obs(&S, 1, obs);
        pol_fn[0](obs, scratch, logits);
    }
    dbg.bench[0] = (long)(TICKS - t0);
    t0 = TICKS;
    for (k = 0; k < 300; k++) {
        for (i = 0; i < S.nt; i++) {
            S.t[i].fire = 1;
            S.t[i].drive = 1;
            S.t[i].turn = 1;
        }
        sim_tick(&S);
    }
    dbg.bench[1] = (long)(TICKS - t0);
    memcpy(&S, &B, sizeof S);
    t0 = TICKS;
    for (k = 0; k < 300; k++) {
        draw_world(0, 0);
        r_present();
    }
    dbg.bench[2] = (long)(TICKS - t0);
    t0 = TICKS;
    for (i = 0; i < S.nt; i++)
        if (S.t[i].alive && ctl[i].kind != C_HUMAN)
            remember(i, 0);
    for (k = 0; k < 100; k++) {
        draw_world(1, 0);
        r_present();
        ai_overlay(1);
    }
    ov_erase();
    for (i = 0; i < 3; i++) {
        ov_parts = 1 << i;
        t0 = TICKS;
        for (k = 0; k < 100; k++) {
            ai_overlay(1);
            dbg.bench2[3 + i] = n_ov;
            ov_erase();
        }
        dbg.bench2[i] = (long)(TICKS - t0);
    }
    ov_parts = 7;
    dbg.bench[3] = (long)(TICKS - t0);
}

/* ------------------------------------------------------------------ stages */
typedef struct { short arena, n, type[3]; } Stage;
static const Stage STAGES[] = {
    {0, 1, {T_RUSHER}},
    {1, 1, {T_SNIPER}},
    {2, 1, {T_CAREFUL}},
    {3, 2, {T_RUSHER, T_RUSHER}},
    {4, 2, {T_SNIPER, T_CAREFUL}},
    {5, 2, {T_RUSHER, T_SNIPER}},
    {6, 3, {T_CAREFUL, T_CAREFUL, T_RUSHER}},
    {7, 3, {T_RUSHER, T_SNIPER, T_CAREFUL}},
};
#define NSTAGES (short)(sizeof STAGES / sizeof STAGES[0])

static void stage_setup(void)
{
    const Stage *st = &STAGES[stage % NSTAGES];
    const unsigned char (*sp)[2] = ARENA_SPAWN[st->arena];
    short k;
    sim_init(&S, st->arena, (uint32_t)(stage * 7919 + 1));
    sim_add_tank(&S, T_PLAYER, 0, sp[0][0], sp[0][1], 0);
    ctl[0].kind = C_HUMAN;
    for (k = 0; k < st->n; k++) {
        short n = k, i;
        unsigned char a;
        if (stage >= NSTAGES && st->n < 3 && k == st->n - 1)
            n = k;
        a = sim_atan2((short)(sp[0][1] - sp[1 + n][1]), (short)(sp[0][0] - sp[1 + n][0]));
        i = sim_add_tank(&S, st->type[k], 1, sp[1 + n][0], sp[1 + n][1], a);
        ctl[i] = ctl_for(st->type[k]);
    }
    if (stage >= NSTAGES && st->n < 3) {       /* the second round: one more enemy */
        short i, n = st->n;
        unsigned char a = sim_atan2((short)(sp[0][1] - sp[1 + n][1]), (short)(sp[0][0] - sp[1 + n][0]));
        static const short extra[3] = {T_SNIPER, T_RUSHER, T_CAREFUL};
        i = sim_add_tank(&S, extra[stage % 3], 1, sp[1 + n][0], sp[1 + n][1], a);
        ctl[i] = ctl_for(extra[stage % 3]);
    }
    S.t[0].hull = S.t[0].tur = S.t[0].tgt = sim_atan2((short)(9 - sp[0][1]), (short)(12 - sp[0][0]));
    memset(fx, 0, sizeof fx);
    memset(last_act, 0, sizeof last_act);
    memset(seen, 0, sizeof seen);
    r_draw_arena((const unsigned char (*)[24])S.map);
}

static void wait_release(void)
{
    while (Button())
        ;
    FlushEvents(everyEvent, 0);
}

/* the stage banner: 1.5 s, or until a key or a click */
static void stage_banner(void)
{
    unsigned char a[40], b[40];
    const char *nm = ARENA_NAME[STAGES[stage % NSTAGES].arena];
    unsigned long t0 = TICKS;
    short k = 0;
    pstr_num(a, "STAGE ", stage + 1);
    while (nm[k]) {
        b[1 + k] = (unsigned char)nm[k];
        k++;
    }
    b[0] = (unsigned char)k;
    dbg.state = 5;
    draw_hud(1);
    sound_play(SND_START);
    while (TICKS - t0 < 100) {
        poll_keys();
        if (housekeeping(1))
            redraw_all();
        draw_world(ai_view, 0);
        banner(a, b);
        present();
        measure();
        sound_tick();
        if (TICKS - t0 > 20 && (PRESSED(K_RETURN) || PRESSED(K_SPACE) || dbg.autoplay))
            break;
        while (TICKS - t0 < 200 && TICKS == t0)
            ;
    }
    timing_reset();
}

/* ------------------------------------------------------------------ the game */
static void play_game(void)
{
    short respawn = 0, clear_t = 0, paused = 0, over_t = 0;
    score = 0;
    lives = 3;
    stage = (short)dbg.start_stage;
    want_new = 0;
    dbg.score = 0;
    stage_setup();
    stage_banner();
    dbg.state = 2;
    for (;;) {
        short n, k, enemies = 0;
        poll_keys();
        if (housekeeping(1))
            redraw_all();
        if (want_new) {
            want_new = 0;
            score = 0;
            lives = 3;
            stage = 0;
            stage_setup();
            stage_banner();
            dbg.state = 2;
            continue;
        }
        if (ESC_PRESSED || want_title) {
            want_title = 0;
            break;
        }
        if (!over_t && (PRESSED(K_RETURN) || PRESSED(K_ENTER) || PRESSED(K_P))) {
            paused = !paused;
            dbg.state = paused ? 3 : 2;
            if (!paused)
                timing_reset();
            else
                sound_stop();
        }
        if (paused) {
            draw_world(ai_view, 0);
            banner("\pPAUSED", "\pReturn or P to go on");
            present();
            sound_tick();
            continue;
        }
        n = ticks_due();
        for (k = 0; k < n; k++) {
            sim_step(1);
            if (!S.t[0].alive && !respawn && !over_t) {
                lives--;
                dbg.lives = lives;
                if (lives > 0)
                    respawn = 60;
                else {
                    over_t = 1;
                    sound_play(SND_OVER);
                    if (score > hiscore) {
                        hiscore = score;
                        prefs_dirty = 1;
                    }
                }
            }
            if (respawn && --respawn == 0) {
                const unsigned char (*sp)[2] = ARENA_SPAWN[STAGES[stage % NSTAGES].arena];
                Tank *t = &S.t[0];
                short x = sp[0][0] * TS + TS / 2, y = sp[0][1] * TS + TS / 2, j, ok = 1;
                for (j = 1; j < S.nt; j++)
                    if (S.t[j].alive && (S.t[j].x - x < 2 * HB && x - S.t[j].x < 2 * HB)
                        && (S.t[j].y - y < 2 * HB && y - S.t[j].y < 2 * HB))
                        ok = 0;
                if (!ok)
                    respawn = 10;          /* an enemy sits on the spawn point: try again soon */
                else {
                    t->alive = 1;
                    t->x = x;
                    t->y = y;
                    t->speed = 0;
                    t->shield = 90;
                    t->by = -1;
                    sound_play(SND_RESPAWN);
                }
            }
        }
        for (k = 1; k < S.nt; k++)
            enemies += S.t[k].alive;
        if (!enemies && !over_t && S.t[0].alive) {
            if (++clear_t == 1) {
                score += 500 + 100 * (long)stage;
                sound_play(SND_CLEAR);
            }
        }
        sound_engine(S.t[0].alive && S.t[0].speed != 0 && !over_t);
        draw_world(ai_view, S.t[0].alive && !over_t && mouse_valid);
        if (clear_t) {
            unsigned char a[40];
            pstr_num(a, "STAGE CLEAR! SCORE ", score);
            banner(a, 0);
        }
        if (over_t) {
            unsigned char a[40];
            pstr_num(a, "SCORE ", score);
            banner("\pGAME OVER", a);
            over_t++;
        }
        present();
        dbg.score = score;
        dbg.stage = stage;
        dbg.lives = lives;
        draw_hud(0);
        measure();
        sound_tick();
        if (clear_t > 60) {
            clear_t = 0;
            stage++;
            if (stage % 4 == 0 && lives < 9)
                lives++;
            stage_setup();
            stage_banner();
            dbg.state = 2;
        }
        if (over_t > 150 || (over_t > 30 && (PRESSED(K_RETURN) || PRESSED(K_SPACE))))
            break;
    }
    sound_stop();
    wait_release();
}

/* ------------------------------------------------------------------ title and attract demo */
static short demo_round;

static void demo_setup(void)
{
    short ar = demo_round % NARENA;
    const unsigned char (*sp)[2] = ARENA_SPAWN[ar];
    short i;
    sim_init(&S, ar, (uint32_t)(12345 + demo_round));
    i = sim_add_tank(&S, T_PLAYER, 0, sp[0][0], sp[0][1], 0);
    ctl[i] = ctl_for(T_PLAYER);
    i = sim_add_tank(&S, T_RUSHER, 1, sp[1][0], sp[1][1], 128);
    ctl[i] = ctl_for(T_RUSHER);
    i = sim_add_tank(&S, T_SNIPER, 1, sp[2][0], sp[2][1], 128);
    ctl[i] = ctl_for(T_SNIPER);
    memset(fx, 0, sizeof fx);
    memset(last_act, 0, sizeof last_act);
    memset(seen, 0, sizeof seen);
    r_draw_arena((const unsigned char (*)[24])S.map);
}

static void title_bg(void)
{
    r_draw_arena((const unsigned char (*)[24])S.map);
    r_logo((AW - LOGO_W) / 2 & ~7, 24);
    fb_text(AW / 2, 96, "\pOutfight the neural-network tanks", 1);
    fb_text(AW / 2, AH - 50, "\pW/S drive, A/D turn, the mouse aims, click fires", 1);
    fb_text(AW / 2, AH - 34, "\pClick or press Return to start", 1);
    fb_text(AW / 2, AH - 18, ai_view ? "\pTab hides the AI's sensors" : "\pTab shows what the AI sees", 1);
    r_commit_bg();
}

static void title(void)
{
    short end_t = 0, saved_diff = difficulty, view_was = -1;
    save_prefs();
    dbg.state = 1;
    demo_round = 0;
    difficulty = 1;              /* the demo always plays the same nets (and the hash check relies on it) */
    demo_setup();
    hud_key = -1;
    draw_hud(1);
    timing_reset();
    wait_release();
    for (;;) {
        short n, k, team0 = 0, team1 = 0;
        poll_keys();
        if (housekeeping(0))
            redraw_all();
        if (view_was != ai_view) {
            view_was = ai_view;
            title_bg();
        }
        if (ESC_PRESSED)
            quit_app();
        if (want_new || PRESSED(K_RETURN) || PRESSED(K_ENTER) || PRESSED(K_SPACE) || dbg.cmd == 1 || dbg.cmd == 4) {
            if (dbg.cmd == 4)
                dbg.autoplay = 1;
            dbg.cmd = 0;
            break;
        }
        if (mouse_down() && mouse_pt().v >= *(short *)0xBAA)
            break;
        if (dbg.cmd == 3) {
            dbg.cmd = 0;
            run_bench();
        }
        n = ticks_due();
        for (k = 0; k < n; k++) {
            short j;
            probe_on = demo_round == 0 && !end_t;
            sim_step(0);
            team0 = team1 = 0;
            for (j = 0; j < S.nt; j++)
                if (S.t[j].alive) {
                    if (S.t[j].team)
                        team1++;
                    else
                        team0++;
                }
            if ((!team0 || !team1 || S.tick >= 1800) && !end_t)
                end_t = 1;
            /* the bit-exactness check (tests/demo_ref.py): every 30 ticks of the first rounds */
            if (S.tick % 30 == 0 && dbg.nhash < 32 && !end_t)
                dbg.hash[dbg.nhash++] = sim_hash(&S);
        }
        if (end_t && ++end_t > 75) {
            end_t = 0;
            demo_round++;
            demo_setup();
            title_bg();
            timing_reset();
        }
        draw_world(ai_view, 0);
        present();
        measure();
        sound_tick();
    }
    difficulty = saved_diff;
    sound_stop();
    wait_release();
}

/* ------------------------------------------------------------------ main */
int main(void)
{
    Rect r;
    dbg.magic = MAGIC;
    dbg.version = 1;
    dbg.notmagic = ~MAGIC;
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
    r = qd.screenBits.bounds;
    r.top += *(short *)0xBAA;
    win = NewWindow(NULL, &r, "\p", true, plainDBox, (WindowPtr)-1, false, 0);
    SetPort(win);
    PaintRect(&win->portRect);
    if (r_init(r.top, r.bottom)) {
        SysBeep(10);
        ExitToShell();
    }
    {
        short y;
        for (y = 0; y < AH; y++)
            ov_row[y] = (unsigned short)(y * r_srb);
    }
    load_prefs();
    setup_menus();
    sound_init();
    sound_set(sound_want);
    dbg.sound = sound_want;
    dbg.ai_view = ai_view;
    dbg.difficulty = difficulty;
    dbg.freemem = FreeMem();
    win_start = TICKS;
    for (;;) {
        title();
        play_game();
    }
    return 0;
}

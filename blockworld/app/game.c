/*
 * Blockworld: the game (M2 walk around, M3 mine and place).
 *
 * 4 MB Mac Plus / SE, System 6 with Narrow Screen. View sizes (the menu): Normal 288x216 (72 4-px
 * columns), Large 384x272, Full the screen's width (448 on a Plus with Narrow Screen, 512 on an SE)
 * x 284; view distance 8 / 10 / 12 blocks; the hotbar under it. While the camera moves, each frame
 * casts the even columns and draws them 8 px wide (r_cast_cols_asm + r_raster8_asm; Full: every
 * 4th column, 16 px wide, r_raster16_asm); once it stops, one refinement casts the rest and draws
 * the full 4-px frame with all edges (Full: in three steps, the input read between them). The crosshair,
 * the outline of the block under it and the mining progress are XORed onto the frame; when only
 * they change, the frame is just presented again.
 *
 * Controls: W A S D move, mouse looks (or the arrow keys: left / right turn, up / down look),
 * Space jumps (walking into a one-block step climbs it by itself), hold the mouse button to mine,
 * E places the selected block or uses a workbench, chest or furnace (Shift-E places against them),
 * Tab opens the inventory, 1-9 select, V view distance, Esc saves and quits.
 * Movement runs at 20 steps per second whatever the frame rate (engine/player.py, ported
 * bit-identically to player.c).
 *
 * A debug block ('BWGM', see Dbg) lets tests/game.lua follow the game.
 */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Events.h>
#include <OSUtils.h>
#include <MacMemory.h>
#include <Segload.h>
#include <string.h>
#include <stdlib.h>
#include "plus.h"
#include "world.h"
#include "render.h"
#include "player.h"
#include "pick.h"
#include "inv.h"
#include "save.h"
#include "obj.h"
#include "mob.h"
#include "drop.h"
#include "sound.h"
#include "tables.h"

#define MAGIC 0x4257474DUL     /* 'BWGM' */
/* the view: Normal 288 x 216, Large 384 x 272, Full (the screen's width, 448 on a Plus with
   Narrow Screen, 512 on an SE) x 284; HUDH rows under it for the status, the hotbar, messages */
static short vnc = 72, vvh = 216, vsize = 0, fullnc = 112;
#define NC vnc                 /* 4-px columns */
#define VW (vnc * 4)
#define VH vvh
#define VW_MAX 512
#define VH_MAX 284
#define SEED 2                  /* the world made when there is no world file at all */
#define SENS 2                 /* yaw / pitch units per mouse pixel */
#define TURN 12                /* yaw per step with the arrow keys (4.2 degrees) */
#define LOOK 3                 /* aim units per step with the arrow keys */
/* Looking up and down: the aim is an angle (1024 a turn, + up) up to AIM_MAX (84 degrees); the
   view's y-shear follows it as 256 tan(aim) rows up to PITCH_R (51 degrees). Beyond that the view
   stays there and slides back from the eye (up to 1.5 blocks, never into a wall) so that what the
   aim's ray hits stays in sight; the ray itself keeps the true angle. */
#define AIM_MAX 240
#define PITCH_R 320
#define AIM_R 146              /* atan(320 / 256) in 1024ths of a turn */
static short aim;
#define HUDH 58                /* the strips under the view: status, hotbar, messages */

#define LM_W(a) (*(volatile short *)(a))
#define LM_B(a) (*(volatile unsigned char *)(a))

struct Dbg {
    unsigned long magic, notMagic;       /*  0,  4 */
    volatile unsigned long state;        /*  8: 1 generating, 2 playing, 0xDEAD no memory */
    unsigned long genTicks;              /* 12 */
    long px, py, pz;                     /* 16: player feet (8.8) */
    short yaw, pitch;                    /* 28 */
    unsigned long frames, moving, refines;   /* 32: frames drawn: all, moving, refinements */
    unsigned long lastFrame;             /* 44: ticks of the last frame (input to present) */
    unsigned long moveTicks, moveCount;  /* 48: moving frames: total ticks and how many timed */
    unsigned long ticks;                 /* 56: Ticks at the top of the last loop */
    short keys, mdx, mdy, autojumps;     /* 60 */
    unsigned long buf;                   /* 68 */
    long freeMem;                        /* 72 */
    short dmax, ground;                  /* 76 */
    unsigned long steps;                 /* 80: physics steps run */
    unsigned long refineTicks;           /* 84: ticks of the last refinement */
    short hit, hx, hy, hz, face, prog;   /* 88: the picked block, mining progress */
    unsigned long mined, placed;         /* 100 */
    Slot hot[NHOT];                      /* 108: the hotbar (item, count, uses left) */
    short sel, pad;                      /* 144 */
    unsigned long presents;              /* 148: overlay-only presents */
    short loaded, loadErr, saveErr;      /* 152: the world came from the save file; errors */
    unsigned long loadTicks;             /* 158: loading (or generating) the world */
    volatile short cmd, arg[3];          /* 162: tests: 1 give arg0 x arg1, 2 stand at (arg0, arg1) facing arg2 */
    short panel;                         /* 170: the panel open (-1 none) */
    unsigned long furnTicks;             /* 172 */
    short panelX, panelY;                /* 176: the panels' top-left (global) */
    unsigned long invAddr, armAddr, chestsAddr, furnsAddr;   /* 180 */
    unsigned long objsAddr;              /* 196 */
    short health, food, air, tod2;       /* 200: (tod / 10) */
    long tod;                            /* 208 */
    short lastKey;                       /* 212: the last key code (| 0x100 in the menu) */
    short spawnOff;                      /* 214: tests: 1 = no natural spawning */
    unsigned long mobsAddr;              /* 216 */
    unsigned long kills;                 /* 220 */
    unsigned long mobFrames;             /* 224: redraws of the sprites alone */
    unsigned long arrowsAddr;            /* 228 */
    short target, atkCool;               /* 232: the mob under the crosshair (-1), attack cooldown */
    short lastHurt, booms;               /* 236: the last damage taken; explosions */
    unsigned long mainAddr;              /* 240: main's address (the PC profiler) */
    unsigned long blocksAddr;            /* 244: bw_blocks */
    short bench;                         /* 248: tests: 1 = a moving frame every loop (timing) */
    unsigned long sndAddr;               /* 250: the sound VBL task (sound.c), then snd_installed */
    unsigned long sndInst;               /* 254 */
    short collectAll;                    /* 258: tests: 1 = dropped items come at once from anywhere */
    unsigned long dropsAddr;             /* 260 */
    short vsize, vw, vh;                 /* 264: the view size (0 Normal, 1 Large, 2 Full) and pixels */
    short aim;                           /* 270: the aim's angle (1024 a turn, + up) */
    short machine;                       /* 272: 0x75 Plus, 0x76 SE (the ROM's version) */
    unsigned long tRead, tUnpack, tWorld, tSave;   /* 274: loading (ticks): reading the file, unpacking it,
                                            building the world; the last save in all */
    volatile short step;                 /* 290: start-up step (tests time them): 1 main, 2 memory, 3 world
                                            ready, 4 sound ready, 5 view ready */
    long seed;                           /* 292: the world's seed */
    short worlds, defErr;                /* 296: new worlds made in the game (menu or test); loading the default */
    unsigned long tPack, tLZ, tWrite;    /* 300: the last save (ticks): packing the world, LZ, writing */
    volatile short genProg;              /* 312: making a world: the progress bar (per mille) */
    unsigned long testBuf;               /* 314: tests: save_test's buffer (64 KB, command 16) */
    short saveTries;                     /* 318: the last save's attempts (3 at most) */
};
static struct Dbg dbg;
static void say(const char *s);
/* an item dropped from block (x, y, z) */
static void drop_at(short item, short n, short x, short y, short z)
{
    drop_spawn((unsigned char)item, n, ((long)x << 8) + 128, ((long)y << 8) + 64, ((long)z << 8) + 128);
}

enum { IN_FWD = 1, IN_BACK = 2, IN_LEFT = 4, IN_RIGHT = 8, IN_JUMP = 16, IN_TURNL = 32, IN_TURNR = 64,
       IN_LOOKU = 128, IN_LOOKD = 256, IN_QUIT = 512, IN_VIEW = 1024, IN_MINE = 2048, IN_PLACE = 4096,
       IN_INV = 8192, IN_SHIFT = 16384 };

static PlView view;
static unsigned char *buf, *tbuf;        /* the frame; the terrain alone (sprites redrawn on it) */
static short mouse_cx, mouse_cy;
static Player pl;

static void *mac_alloc(long n) { return NewPtr(n); }     /* (not cleared: the world fills it all) */

/* Held keys come from the key map; one-off presses (E, Tab, V, Esc, 1-9) from the event queue,
   so that none is lost while a slow frame is drawn. */
static short read_input(short *mdx, short *mdy, short *num)
{
    static const unsigned char numkey[NHOT] = {0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19};
    unsigned char km[16];
    short in = 0, i;
    EventRecord ev;
#define KEY(c) ((km[(c) >> 3] >> ((c) & 7)) & 1)
    GetKeys(km);
    if (KEY(0x0D)) in |= IN_FWD;                                  /* W */
    if (KEY(0x01)) in |= IN_BACK;                                 /* S */
    if (KEY(0x00)) in |= IN_LEFT;                                 /* A */
    if (KEY(0x02)) in |= IN_RIGHT;                                /* D */
    if (KEY(0x31)) in |= IN_JUMP;                                 /* Space */
    if (KEY(0x46) || KEY(0x7B)) in |= IN_TURNL;                   /* arrows: Plus, ADB */
    if (KEY(0x42) || KEY(0x7C)) in |= IN_TURNR;
    if (KEY(0x4D) || KEY(0x7E)) in |= IN_LOOKU;
    if (KEY(0x48) || KEY(0x7D)) in |= IN_LOOKD;
    if (KEY(0x38)) in |= IN_SHIFT;                                /* Shift: E places even on a bench */
#undef KEY
    *num = -1;
    FlushEvents(mDownMask | mUpMask, 0);
    while (GetNextEvent(keyDownMask, &ev)) {
        short k = (short)((ev.message & keyCodeMask) >> 8);
        dbg.lastKey = k;
        if (k == 0x09) in |= IN_VIEW;                              /* V */
        else if (k == 0x2E) { snd_toggle(); say(snd_on ? "Sound on." : "Sound off."); }   /* M */
        else if (k == 0x0E) in |= IN_PLACE;                        /* E */
        else if (k == 0x30) in |= IN_INV;                          /* Tab */
        /* Esc: 0x35 on ADB keyboards (SE); the Plus has none, emu/play.sh maps the host's Esc to
           its backquote key, 0x32. Cmd-Q too. */
        else if (k == 0x35 || k == 0x32 || (k == 0x0C && (ev.modifiers & cmdKey))) in |= IN_QUIT;
        else
            for (i = 0; i < NHOT; i++)
                if (k == numkey[i]) *num = i;
        if (k == 0x0E && (ev.modifiers & shiftKey)) in |= IN_SHIFT;
    }
    if (Button()) in |= IN_MINE;
    /* the mouse: movement since the last frame, then re-centre the cursor */
    *mdx = LM_W(0x82A) - mouse_cx;
    *mdy = LM_W(0x828) - mouse_cy;
    LM_W(0x828) = mouse_cy; LM_W(0x82A) = mouse_cx;
    LM_W(0x82C) = mouse_cy; LM_W(0x82E) = mouse_cx;
    LM_B(0x8CE) = LM_B(0x8CF);
    return in;
}

/* ---- overlays on the frame (XOR, in the column-pair buffer) ---------------------------- */

static void plot(short x, short y)
{
    short b = x >> 3;
    buf[(long)(b >> 1) * 2 * VH + 2 * y + (b & 1)] ^= (unsigned char)(0x80 >> (x & 7));
}

static void crosshair_at(short cx, short cy)
{
    short i;
    for (i = 2; i <= 5; i++) {
        plot((short)(cx + i), cy); plot((short)(cx - i), cy);
        plot(cx, (short)(cy + i)); plot(cx, (short)(cy - i));
    }
    plot(cx, cy);
}

/* the mining progress: a bar of n pixels (0..20) under the crosshair */
/* the mining progress under the crosshair: a framed bar, black on white (n of 20), drawn over the
   frame (not XOR, so it shows on any texture); n 0: nothing */
static void setpx(short x, short y, short black)
{
    short b = x >> 3;
    unsigned char *p = &buf[(long)(b >> 1) * 2 * VH + 2 * y + (b & 1)], m = (unsigned char)(0x80 >> (x & 7));
    if (black) *p |= m; else *p &= (unsigned char)~m;
}
/* the aim's target off the view (below or above it): an arrow at that edge and the block's
   icon beside it, in a white box */
static void edge_marker(short below, short item)
{
    short x, y, cx = VW / 2, y0 = below ? (short)(VH - 22) : 2;
    short xl = item ? -22 : -11, xr = item ? 21 : 10, a0 = item ? 11 : 0;   /* (no block: the arrow alone) */
    for (y = 0; y < 20; y++)
        for (x = xl; x <= xr; x++) {
            short on = (short)(x == xl || x == xr || y == 0 || y == 19);       /* the frame */
            short ay = below ? y : (short)(19 - y), ax = (short)(x + a0);      /* the arrow, at x = -a0 */
            if (ay >= 3 && ay < 9 && ax >= -2 && ax <= 2) on = 1;            /* its shaft */
            if (ay >= 9 && ay < 17 && ax >= -(16 - ay) && ax <= 16 - ay) on = 1;   /* its head */
            if (item && x >= 3 && x < 19 && y >= 2 && y < 18)               /* the block's icon */
                on = (short)((item_icon[item][y - 2] >> (15 - (x - 3))) & 1);
            setpx((short)(cx + x), (short)(y0 + y), on);
        }
}

static void progress_at(short n, short cx, short cy)
{
    short x, y, x0 = (short)(cx - 12), y0 = (short)(cy + 8);
    if (n <= 0) return;
    for (y = 0; y < 6; y++)
        for (x = 0; x < 24; x++)
            setpx((short)(x0 + x), (short)(y0 + y),
                  (short)(y == 0 || y == 5 || x == 0 || x == 23 || (y >= 2 && y <= 3 && x >= 2 && x < 2 + n)));
}

static int pick_solid(int x, int y, int z)
{
    int t;
    if (x < 0 || x >= WX || z < 0 || z >= WX || y >= WY) return 0;
    if (y < 0) return 0;
    t = bw_blocks[BIDX(x, z, y)];
    return (t != AIR && t != WATER) || (t == AIR && obj_at((short)x, (short)y, (short)z) >= 0);
}

static unsigned long rnd_s = 0x2545F491UL;
static unsigned short rnd(void)            /* xorshift */
{
    rnd_s ^= rnd_s << 13; rnd_s ^= rnd_s >> 17; rnd_s ^= rnd_s << 5;
    return (unsigned short)(rnd_s >> 8);
}

/* ---- torch light: the columns within TORCH_R (x + z steps) of a torch are lit over the blocks
   within TORCH_R (x + y + z steps); world.c gives the segments there light class 3 */
#define TORCH_R 4
static short iabs(short v) { return v < 0 ? (short)-v : v; }
static void lit_add(short tx, short ty, short tz)
{
    short xx, zz;
    for (xx = (short)(tx - TORCH_R); xx <= tx + TORCH_R; xx++)
        for (zz = (short)(tz - TORCH_R); zz <= tz + TORCH_R; zz++) {
            short r = (short)(TORCH_R - iabs((short)(xx - tx)) - iabs((short)(zz - tz))), lo, hi;
            unsigned char *l;
            if (r < 0 || xx < 0 || xx >= WX || zz < 0 || zz >= WX) continue;
            lo = (short)(ty - r); hi = (short)(ty + r + 1);
            if (lo < 0) lo = 0;
            if (hi > WY) hi = WY;
            l = bw_lit[xx * WX + zz];
            if (!l[1]) { l[0] = (unsigned char)lo; l[1] = (unsigned char)hi; continue; }
            if (lo < l[0]) l[0] = (unsigned char)lo;
            if (hi > l[1]) l[1] = (unsigned char)hi;
        }
}
/* every torch's light (before world_build_segments) */
static void light_all(void)
{
    short i;
    memset(bw_lit, 0, sizeof bw_lit);
    for (i = 0; i < NOBJ; i++)
        if (objs[i].type == O_TORCH) lit_add(objs[i].x, objs[i].y, objs[i].z);
}
/* a torch placed or gone at (x, z): the light around it again, from the torches near it */
static void relight(short x, short z)
{
    static unsigned char old[2 * TORCH_R + 1][2 * TORCH_R + 1][2];
    short xx, zz, i;
    for (xx = (short)(x - TORCH_R); xx <= x + TORCH_R; xx++)
        for (zz = (short)(z - TORCH_R); zz <= z + TORCH_R; zz++)
            if (xx >= 0 && xx < WX && zz >= 0 && zz < WX) {
                unsigned char *l = bw_lit[xx * WX + zz], *o = old[xx - x + TORCH_R][zz - z + TORCH_R];
                o[0] = l[0]; o[1] = l[1];
                l[0] = l[1] = 0;
            }
    for (i = 0; i < NOBJ; i++) {                       /* the torches whose light reaches there */
        const Obj *o = &objs[i];
        if (o->type == O_TORCH && iabs((short)(o->x - x)) <= 2 * TORCH_R && iabs((short)(o->z - z)) <= 2 * TORCH_R)
            lit_add(o->x, o->y, o->z);
    }
    for (xx = (short)(x - TORCH_R); xx <= x + TORCH_R; xx++)
        for (zz = (short)(z - TORCH_R); zz <= z + TORCH_R; zz++)
            if (xx >= 0 && xx < WX && zz >= 0 && zz < WX) {
                unsigned char *l = bw_lit[xx * WX + zz], *o = old[xx - x + TORCH_R][zz - z + TORCH_R];
                if (o[0] != l[0] || o[1] != l[1]) world_column_changed(xx, zz);
            }
}
/* remove an object (a torch: its light goes) */
static void remove_obj(short i)
{
    short tx = objs[i].x, tz = objs[i].z, torch = objs[i].type == O_TORCH;
    obj_remove(i);
    if (torch) relight(tx, tz);
}

/* the item an object gives back when broken, and how many */
static short obj_drop(short i, short *n)
{
    const Obj *o = &objs[i];
    *n = 1;
    if (o->type == O_TORCH) return I_TORCH;
    if (o->type == O_PLANT && o->state < 16) return I_SAPLING;
    if (o->type == O_PLANT && o->state >= 23) { *n = 1; return I_WHEAT; }
    if (o->type == O_PLANT) return I_SEEDS;
    return 0;
}

/* a block is gone: an object standing on it falls and is given back */
static void support_gone(short x, short y, short z)
{
    short i = obj_at(x, (short)(y + 1), z), n, it;
    if (i < 0) return;
    it = obj_drop(i, &n);
    if (it) drop_at(it, n, x, (short)(y + 1), z);
    remove_obj(i);
}

/* sand and gravel with nothing under them fall to the next solid block (at once) */
static short settle(short x, short z)
{
    short y, moved = 0;
    if (x < 0 || x >= WX || z < 0 || z >= WX) return 0;
    for (y = 1; y < WY; y++) {
        unsigned char t = bw_blocks[BIDX(x, z, y)], u = bw_blocks[BIDX(x, z, y - 1)];
        if ((t == SAND || t == GRAVEL) && (u == AIR || u == WATER || u == LAVA)) {
            short yy = (short)(y - 1), oi;
            while (yy > 0) {
                u = bw_blocks[BIDX(x, z, yy - 1)];
                if (u != AIR && u != WATER && u != LAVA) break;
                yy--;
            }
            oi = obj_at(x, yy, z);
            if (oi >= 0) { short n, it = obj_drop(oi, &n); if (it) drop_at(it, n, x, yy, z); remove_obj(oi); }
            bw_blocks[BIDX(x, z, yy)] = t;
            bw_blocks[BIDX(x, z, y)] = AIR;
            support_gone(x, y, z);
            moved = 1;
        }
    }
    if (moved) world_column_changed(x, z);
    return moved;
}

/* a tree from a sapling at (x, y, z) (its trunk's lowest block), as world.c's generator */
static void grow_tree(short x, short y, short z)
{
    short th = (short)(4 + (rnd() & 1)), top = (short)(y + th - 1), yy, xx, zz;
    if (top + 2 >= WY || x < 2 || x >= WX - 2 || z < 2 || z >= WX - 2) return;
    for (yy = (short)(top - 2); yy < top + 2; yy++) {
        short rad = yy < top ? 2 : 1;
        for (xx = (short)(x - rad); xx <= x + rad; xx++)
            for (zz = (short)(z - rad); zz <= z + rad; zz++) {
                short corner = (xx - x == rad || x - xx == rad) && (zz - z == rad || z - zz == rad);
                if (corner && (rad == 1 || (rnd() & 1))) continue;
                if (bw_blocks[BIDX(xx, zz, yy)] == AIR && obj_at(xx, yy, zz) < 0) bw_blocks[BIDX(xx, zz, yy)] = LEAVES;
            }
    }
    for (yy = y; yy <= top; yy++) bw_blocks[BIDX(x, z, yy)] = LOG;
    for (xx = (short)(x - 2); xx <= x + 2; xx++)
        for (zz = (short)(z - 2); zz <= z + 2; zz++) world_column_changed(xx, zz);
}

/* plants grow: a sapling in about 2 minutes, a crop in about 3 (random steps) */
static short plants_tick(void)
{
    short i = (short)(rnd() % NOBJ), changed = 0;
    Obj *o = &objs[i];
    if (o->type != O_PLANT || rnd() % 64) return 0;
    if (o->state < 3 || (o->state >= 16 && o->state < 23)) { o->state++; changed = 1; }
    else if (o->state == 3) {
        short x = o->x, y = o->y, z = o->z;
        obj_remove(i);
        grow_tree(x, y, z);
        changed = 1;
    }
    return changed;
}

static short held_item(void) { return inv[inv_sel].count ? inv[inv_sel].item : 0; }

static short bar_len(const Pick *pk, short prog)
{
    short h = pk->hit ? mine_time(bw_blocks[BIDX(pk->x, pk->z, pk->y)], held_item()) : 0;
    return h ? (short)(prog * 20 / h) : 0;
}


static void furn_poll(void)                /* furnaces keep cooking while a panel is open */
{
    static unsigned long last;
    unsigned long now = pl_ticks();
    if (!last) last = now;
    while (now - last >= 3) { furnaces_tick(); last += 3; dbg.furnTicks++; }
}

static void message(const char *s1, const char *s2)
{
    Rect r = qd.screenBits.bounds;
    short cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    Str255 p;
    FillRect(&r, &qd.black);
    TextFont(0);
    TextMode(srcBic);
    p[0] = (unsigned char)strlen(s1); memcpy(p + 1, s1, p[0]);
    MoveTo(cx - StringWidth(p) / 2, cy - 8);
    DrawString(p);
    p[0] = (unsigned char)strlen(s2); memcpy(p + 1, s2, p[0]);
    MoveTo(cx - StringWidth(p) / 2, cy + 12);
    DrawString(p);
}

/* the height of the highest block of column (x, z), + 1 */
static short top_of(short x, short z)
{
    short h = WY - 1;
    if (x < 0 || x >= WX || z < 0 || z >= WX) return WY;
    while (h > 0 && bw_blocks[BIDX(x, z, h)] == AIR) h--;
    return (short)(h + 1);
}

/* the direction (yaw) with the longest walkable stretch from column (x, z) */
static short open_yaw(short x, short z)
{
    static const short dx[4] = {1, 0, -1, 0}, dz[4] = {0, 1, 0, -1};
    short d, best = 0, bestn = -1, h0 = top_of(x, z);
    for (d = 0; d < 4; d++) {
        short n = 0, h = h0;
        while (n < 12) {
            short t = top_of((short)(x + dx[d] * (n + 1)), (short)(z + dz[d] * (n + 1)));
            if (t > h + 1 || t < h - 3) break;
            h = t;
            n++;
        }
        if (n > bestn) { bestn = n; best = d; }
    }
    return (short)(best * 256);
}

/* a spawn point: the grass column nearest the world's centre with no tree on it */
static void spawn(long *x, long *y, long *z)
{
    short r, i, j;
    for (r = 0; r < WX / 2; r++)
        for (i = -r; i <= r; i++)
            for (j = -r; j <= r; j++) {
                short cx = (short)(WX / 2 + i), cz = (short)(WX / 2 + j), h = WY - 1;
                if (i != -r && i != r && j != -r && j != r) continue;
                while (h > 0 && bw_blocks[BIDX(cx, cz, h)] == AIR) h--;
                if (bw_blocks[BIDX(cx, cz, h)] == TURF && h < WY - 6) {
                    *x = ((long)cx << 8) + 128; *z = ((long)cz << 8) + 128;
                    *y = (long)(h + 1) << 8;
                    return;
                }
            }
    *x = (long)(WX / 2) << 8; *z = *x; *y = (long)SEA << 8;
}

/* does the player's box overlap block (x, y, z)? */
static int player_in(short x, short y, short z)
{
    long bx = (long)x << 8, by = (long)y << 8, bz = (long)z << 8;
    return pl.x + PL_HW > bx && pl.x - PL_HW < bx + 256 && pl.z + PL_HW > bz && pl.z - PL_HW < bz + 256
        && pl.y + PL_HT > by && pl.y < by + 256;
}

/* ---- survival ---------------------------------------------------------------------------- */

#define DAY 24000                /* a day: 20 minutes of 20 Hz steps; 0 sunrise, 6000 noon */
#define NIGHT(t) ((t) >= 13000 && (t) < 23000)
typedef struct Life {
    short health, food, air, hungerT, regenT, lavaT, airborne, sky, dead;
    long peak, tod;
    short bed, bx, by, bz;       /* the respawn point (a bed) */
    long sx, sy, sz;             /* the world spawn */
} Life;
static Life life;
static char msg[64];
static short msg_t;              /* steps the message stays */

static void say(const char *s)
{
    short n = 0;
    while (s[n] && n < 63) { msg[n] = s[n]; n++; }
    msg[n] = 0;
    hud_message(msg);
    msg_t = 120;                 /* 6 s */
}

static void hurt(short n)
{
    short pts = armour_points();
    n = (short)((n * (25 - pts) + 24) / 25);
    if (pts) armour_wear();
    life.health = (short)(life.health - n);
    dbg.lastHurt = n;
    if (n > 0) snd_play(FX_HURT);
}

static short block_at(long x, long y, long z)
{
    short bx = (short)(x >> 8), by = (short)(y >> 8), bz = (short)(z >> 8);
    if (bx < 0 || bx >= WX || bz < 0 || bz >= WX || by < 0 || by >= WY) return AIR;
    return bw_blocks[BIDX(bx, bz, by)];
}

/* one 1/20 s step of the body: air, lava, falling, hunger, healing; the clock */
static void life_tick(short moving)
{
    life.tod = (life.tod + 1) % DAY;
    if (block_at(pl.x, pl.y + PL_EYE, pl.z) == WATER) {               /* holding the breath */
        if (--life.air < -20) { hurt(2); life.air = 0; }
    } else
        life.air = 300;
    if (block_at(pl.x, pl.y, pl.z) == LAVA || block_at(pl.x, pl.y + 256, pl.z) == LAVA) {
        if (--life.lavaT <= 0) { hurt(4); life.lavaT = 10; }
    } else
        life.lavaT = 0;
    if (!pl.ground) {                                                  /* falling: from the top */
        if (!life.airborne || pl.y > life.peak) life.peak = pl.y;
        life.airborne = 1;
        if (block_at(pl.x, pl.y, pl.z) == WATER || block_at(pl.x, pl.y, pl.z) == LADDER) life.peak = pl.y;
    } else if (life.airborne) {
        long fall = life.peak - pl.y - 3 * 256;                       /* blocks beyond 3, rounded up */
        life.airborne = 0;
        if (fall > 0) hurt((short)((fall + 255) >> 8));
    }
    life.hungerT = (short)(life.hungerT + (moving ? 2 : 1));
    if (life.hungerT >= 1600) { life.hungerT = 0; if (life.food > 0) life.food--; }
    if (life.food >= 18 && life.health < 20) {
        if (++life.regenT >= 80) { life.regenT = 0; life.health++; }
    } else if (life.food == 0 && life.health > 1) {
        if (++life.regenT >= 80) { life.regenT = 0; life.health--; }
    } else
        life.regenT = 0;
    if (msg_t && !--msg_t) hud_message(0);
}

/* death: the inventory goes into chests where the player died; back to the bed or the spawn */
static void die(void)
{
    short x = (short)(pl.x >> 8), y = (short)(pl.y >> 8), z = (short)(pl.z >> 8), k = 0, i, c;
    Slot all[NINV + NARM];
    for (i = 0; i < NINV; i++) all[i] = inv[i];
    for (i = 0; i < NARM; i++) all[NINV + i] = inv_arm[i];
    memset(inv, 0, sizeof inv);
    memset(inv_arm, 0, sizeof inv_arm);
    if (y < 1) y = 1;
    for (c = 0; c < 2 && k < NINV + NARM; c++) {
        Chest *ch;
        short yy = y;
        while (yy < WY - 1 && bw_blocks[BIDX(x, z, yy)] != AIR) yy++;
        while (k < NINV + NARM && !all[k].count) k++;
        if (k >= NINV + NARM || x < 0 || x >= WX || z < 0 || z >= WX) break;
        ch = chest_at(x, yy, z, 1);
        if (!ch) break;
        bw_blocks[BIDX(x, z, yy)] = CHEST;
        world_column_changed(x, z);
        for (i = 0; i < 27 && k < NINV + NARM; k++)
            if (all[k].count) ch->s[i++] = all[k];
        y = (short)(yy + 1);
    }
    if (life.bed) player_init(&pl, ((long)life.bx << 8) + 128, (long)life.by << 8, ((long)life.bz << 8) + 128, pl.yaw);
    else player_init(&pl, life.sx, life.sy, life.sz, pl.yaw);
    life.health = 20; life.food = 20; life.air = 300; life.airborne = 0;
    say("You died. Your things are in a chest where you fell.");
}

/* the sky by the time of day: 0 day, 1 dusk or dawn, 2 night */
static const unsigned char SKY_DAY[8] = {0}, HAZE_DAY[8] = {0, 0, 0, 0, 0x40, 0, 0, 0},
    FOG_DAY[8] = {0, 0, 0, 0, 0x40, 0, 0x04, 0};
static const unsigned char SKY_DUSK[8] = {0x88, 0, 0x22, 0, 0x88, 0, 0x22, 0},
    HAZE_DUSK[8] = {0x88, 0x22, 0x88, 0x22, 0x88, 0x22, 0x88, 0x22}, FOG_DUSK[8] = {0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55};
static const unsigned char SKY_NIGHT[8] = {0xFF, 0xFF, 0xFB, 0xFF, 0xFF, 0xFF, 0xFF, 0xBF},
    HAZE_NIGHT[8] = {0xEE, 0xFF, 0xBB, 0xFF, 0xEE, 0xFF, 0xBB, 0xFF}, FOG_NIGHT[8] = {0xDD, 0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD, 0x77};
static short sky_phase(long t)
{
    return NIGHT(t) ? 2 : (t >= 12000 && t < 13000) || t >= 23000 ? 1 : 0;
}
/* the light: the dither for each light class (open sky, under a tree, covered, torch-lit) */
static const unsigned char SHADE[3][4] = {{0, 2, 3, 0}, {2, 2, 3, 0}, {2, 3, 3, 0}};
/* the distant hills (nearer, farther) and their edge by day, at dusk and at night */
static const unsigned char HILLS[3][3][8] = {
    {{0x88, 0, 0x20, 0, 0x88, 0, 0x02, 0}, {0x88, 0, 0, 0, 0x88, 0, 0, 0}, {0x22, 0x88, 0x22, 0x88, 0x22, 0x88, 0x22, 0x88}},
    {{0xAA, 0x44, 0xAA, 0, 0xAA, 0x44, 0xAA, 0}, {0xAA, 0, 0xAA, 0, 0xAA, 0, 0xAA, 0}, {0x22, 0x88, 0x22, 0x88, 0x22, 0x88, 0x22, 0x88}},
    {{0xEE, 0x55, 0xBB, 0x55, 0xEE, 0x55, 0xBB, 0x55}, {0xEE, 0x55, 0xAA, 0x55, 0xEE, 0x55, 0xAA, 0x55}, {0, 0, 0, 0, 0, 0, 0, 0}}};
static void set_sky(short ph)
{
    if (ph == 0) r_set_sky(SKY_DAY, HAZE_DAY, FOG_DAY);
    else if (ph == 1) r_set_sky(SKY_DUSK, HAZE_DUSK, FOG_DUSK);
    else r_set_sky(SKY_NIGHT, HAZE_NIGHT, FOG_NIGHT);
    r_set_pat(BW_FARHILL, HILLS[ph][0]);
    r_set_pat(BW_FARHILL + 1, HILLS[ph][1]);
    r_set_pat(BW_FARLINE, HILLS[ph][2]);
    r_set_shade(SHADE[ph]);
    life.sky = ph;
}

static const char *const sun_pic[10] = {
    "...####...", ".##oooo##.", ".#oooooo#.", "#oooooooo#", "#oooooooo#", "#oooooooo#", "#oooooooo#",
    ".#oooooo#.", ".##oooo##.", "...####..."};
static const char *const moon_pic[10] = {
    "...ooo....", ".oooo.....", ".ooo......", "oooo......", "ooo.......", "ooo.......", "oooo......",
    ".ooo......", ".oooo.....", "...ooo...."};

/* the sun and the moon, 96 blocks away (behind the distant hills, in front of the sky) */
static void sky_draw(const RCam *cam, short moving)
{
    short a = (short)(life.tod * 1024 / DAY), k;
    for (k = 0; k < 2; k++) {
        short ang = (short)((a + k * 512) & 1023);
        Sprite sp;
        long cx = bw_sin[(ang + 256) & 1023], sy = bw_sin[ang];
        if (sy < -2000) continue;                                      /* under the horizon */
        sp.x = cam->x + cx * 96 * 256 / 16384;
        sp.z = cam->z;
        sp.y = cam->y + sy * 96 * 256 / 16384 - 1024;
        sp.sw = 2048; sp.sh = 2048;
        sp.pic = k ? moon_pic : sun_pic; sp.pw = 10; sp.ph = 10;
        sp.flip = 0; sp.solid = 0; sp.dim = 0;
        sprite_draw(cam, &sp, buf, VW, VH, moving);
    }
}

/* ---- mobs: the game's side ------------------------------------------------------------- */
static MobWorld mw;
static short kvx, kvz;                   /* the player's knockback (1/256 block per step) */

static void mw_hurt(short dmg, long fromx, long fromz)
{
    short a = yaw_to(pl.x - fromx, pl.z - fromz);
    hurt(dmg);
    kvx = (short)(((long)bw_sin[(a + 256) & 1023] * 56) >> 14);
    kvz = (short)(((long)bw_sin[a & 1023] * 56) >> 14);
    if (pl.ground) { pl.vy = 50; pl.ground = 0; }
}
static void mw_drop(unsigned char item, short n, long x, long y, long z) { drop_spawn(item, n, x, y, z); }

/* a sizzler goes off: blocks within about 2 blocks break (not bedrock or liquids), anything near
   is hurt by distance */
static void explode(long x, long y, long z)
{
    short bx = (short)(x >> 8), by = (short)(y >> 8), bz = (short)(z >> 8), dx, dy, dz, i;
    long d;
    for (dx = -2; dx <= 2; dx++)
        for (dz = -2; dz <= 2; dz++) {
            short cx = (short)(bx + dx), cz = (short)(bz + dz), changed = 0;
            if (cx < 0 || cx >= WX || cz < 0 || cz >= WX) continue;
            for (dy = -2; dy <= 2; dy++) {
                short cy = (short)(by + dy), t, oi;
                if (cy < 1 || cy >= WY || dx * dx + dy * dy + dz * dz > 5) continue;
                oi = obj_at(cx, cy, cz);
                if (oi >= 0) remove_obj(oi);
                t = bw_blocks[BIDX(cx, cz, cy)];
                if (t == AIR || t == BEDROCK || t == WATER || t == LAVA) continue;
                if (t == CHEST || t == FURNACE) container_broken(cx, cy, cz);
                bw_blocks[BIDX(cx, cz, cy)] = AIR;
                changed = 1;
            }
            if (changed) {
                world_column_changed(cx, cz);
                for (dy = -2; dy <= 3; dy++) support_gone(cx, (short)(by + dy), cz);
                settle(cx, cz);
            }
        }
    d = (labs(pl.x - x) + labs(pl.z - z) + labs(pl.y + 256 - y)) >> 8;     /* blocks, roughly */
    if (d < 5) mw_hurt((short)(16 - 3 * d), x, z);
    for (i = 0; i < NMOB; i++)
        if (mobs[i].alive) {
            long e = (labs(mobs[i].x - x) + labs(mobs[i].z - z) + labs(mobs[i].y - y)) >> 8;
            if (e < 4) mob_hurt(i, (short)(14 - 3 * e), yaw_to(mobs[i].x - x, mobs[i].z - z), &mw);
        }
    dbg.booms++;
    snd_play(FX_BOOM);
}

/* the damage of a hit with what's in hand: swords best, then axes */
static short attack_damage(void)
{
    static const short sword[5] = {4, 5, 6, 4, 7}, axe[5] = {3, 4, 5, 3, 6};
    short it = held_item();
    if (!IS_TOOL(it)) return 1;
    if (TOOL_KIND(it) == K_SWORD) return sword[TOOL_TIER(it)];
    if (TOOL_KIND(it) == K_AXE) return axe[TOOL_TIER(it)];
    return 2;
}

/* take one of an item from the inventory (0: none) */
static short take_one(unsigned char item)
{
    short i;
    for (i = 0; i < NINV; i++)
        if (inv[i].count && inv[i].item == item) {
            if (!--inv[i].count) inv[i].item = 0;
            return 1;
        }
    return 0;
}

/* the y-shear (rows) for an aim angle, and the angle for a y-shear */
static short pitch_rows(short a)
{
    long s = bw_sin[a & 1023], c = bw_sin[(a + 256) & 1023], r;
    if (c < 64) return (short)(a < 0 ? -PITCH_R : PITCH_R);
    r = (s * 256) / c;
    return (short)(r > PITCH_R ? PITCH_R : r < -PITCH_R ? -PITCH_R : r);
}
static short aim_of_rows(short rows)
{
    short a = 0, best = 0;
    long bd = 1L << 30;
    for (a = -AIM_R; a <= AIM_R; a++) {
        long d = labs((long)pitch_rows(a) - rows);
        if (d < bd) { bd = d; best = a; }
    }
    return best;
}
static void set_aim(short a)
{
    aim = (short)(a > AIM_MAX ? AIM_MAX : a < -AIM_MAX ? -AIM_MAX : a);
    pl.pitch = pitch_rows(aim);
}

/* the view at size k: renderer tables, the screen cleared, the HUD under it */
static void set_view(short k, short dmax)
{
    static const short size_h[3] = {216, 272, 284};
    vsize = k;
    vnc = k == 0 ? 72 : k == 1 ? 96 : fullnc;
    vvh = size_h[k];
    r_config(NC, VH, dmax);
    if (life.sky >= 0) set_sky(life.sky);
    FillRect(&qd.screenBits.bounds, &qd.black);
    pl_view_init(&view, VW, VH);
    {   /* the view moves up to leave room for the hotbar strip */
        short up = HUDH / 2;
        view.base -= (long)up * view.rowBytes;
        OffsetRect(&view.rect, 0, (short)-up);
        hud_init((unsigned char *)qd.screenBits.baseAddr, (short)(qd.screenBits.rowBytes & 0x3FFF),
                 (short)((view.rect.left + view.rect.right) / 2 - NHOT * 12 - qd.screenBits.bounds.left),
                 (short)(view.rect.bottom + 3 - qd.screenBits.bounds.top));
    }
    hud_hotbar();
    hud_status(life.health, life.food, life.air / 15 > 20 ? 20 : life.air / 15);
    dbg.panelX = (short)(view.rect.left + (VW - 288) / 2); dbg.panelY = (short)(view.rect.top + (VH - 216) / 2);
    dbg.vsize = k; dbg.vw = VW; dbg.vh = VH; dbg.dmax = dmax;
}
/* the 288 x 216 panels and menu, centred in the view (screen-memory coordinates) */
#define PANEL_X ((short)(view.rect.left - qd.screenBits.bounds.left + (VW - 288) / 2))
#define PANEL_Y ((short)(view.rect.top - qd.screenBits.bounds.top + (VH - 216) / 2))

/* the camera the view is drawn from: the eye, or, aiming beyond PITCH_R, slid back so the aim's
   target (pk: its distance along the ray) shows at the centre */
static void render_cam(RCam *rc, const RCam *eye, const Pick *pk)
{
    short a = (short)(aim < 0 ? -aim : aim);
    *rc = *eye;
    if (a > AIM_R) {
        long k = (pk->t * bw_sin[(a - AIM_R) & 1023]) / bw_sin[AIM_R], step;
        long c = bw_sin[(eye->yaw + 256) & 1023], s = bw_sin[eye->yaw & 1023];
        if (k > 384) k = 384;
        for (step = 32; step <= k; step += 32)                 /* not into a wall */
            if (player_solid((int)((eye->x - ((c * step) >> 14)) >> 8), (int)((eye->z - ((s * step) >> 14)) >> 8),
                             (int)(eye->y >> 8))) { k = step - 32; break; }
        rc->x -= (c * k) >> 14;
        rc->z -= (s * k) >> 14;
    }
}

/* where the aim's target shows in the view drawn from rc: 1 if inside it */
static short aim_point(const RCam *rc, const RCam *eye, const Pick *pk, short *cx, short *cy)
{
    long ca = bw_sin[(aim + 256) & 1023], sa = bw_sin[aim & 1023];
    long c = bw_sin[(eye->yaw + 256) & 1023], s = bw_sin[eye->yaw & 1023];
    long t = pk->t, px, py, pz, rx, rz, d, l, y;
    short a = (short)(aim < 0 ? -aim : aim);
    *cx = VW / 2; *cy = VH / 2;
    if (a <= AIM_R) return 1;                                   /* the centre ray */
    px = eye->x + ((((c * ca) >> 14) * t) >> 14);
    pz = eye->z + ((((s * ca) >> 14) * t) >> 14);
    py = eye->y + ((sa * t) >> 14);
    rx = px - rc->x; rz = pz - rc->z;
    d = (rx * c + rz * s) >> 14;
    if (d < 16) { *cy = aim < 0 ? VH : -1; return 0; }
    l = (rz * c - rx * s) >> 14;
    y = VH / 2 + rc->pitch - (py - rc->y) * 256 / d;
    *cx = (short)(VW / 2 + l * 256 / d);
    if (y < 8 || y > VH - 14) { *cy = (short)(y < 8 ? -1 : VH); return 0; }
    *cy = (short)y;
    return 1;
}

/* the crosshair (or an edge marker), the outline and the mining bar over the frame */
static void overlays(const RCam *rc, const RCam *eye, const Pick *pk, short n)
{
    short cx, cy, in = aim_point(rc, eye, pk, &cx, &cy);
    if (cx < 14) cx = 14;
    if (cx > VW - 14) cx = (short)(VW - 14);
    if (in) crosshair_at(cx, cy);
    pick_outline(rc, pk, buf, VW, VH);
    if (!in) edge_marker((short)(cy >= VH), (short)(pk->hit ? bw_blocks[BIDX(pk->x, pk->z, pk->y)] : 0));
    progress_at(n, (short)(in ? cx : VW / 2), (short)(in ? cy : cy >= VH ? VH - 40 : 30));
}

static void set_cam(RCam *c)
{
    c->x = pl.x; c->y = pl.y + PL_EYE; c->z = pl.z; c->yaw = pl.yaw; c->pitch = pl.pitch;
}

/* ---- worlds: load, make, save (start-up and the menu's New world / Starting world) ---------- */

static long world_seed;
static short prog_base, prog_span, prog_shown;

/* the progress bar under the message (per mille of the job), white on black */
static void progress_bar(short v)
{
    Rect r = qd.screenBits.bounds, b;
    short cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, w;
    if (v < 0) v = 0;
    if (v > 1000) v = 1000;
    w = (short)((long)v * 196 / 1000);
    if (prog_shown >= 0 && w == prog_shown) return;
    SetRect(&b, cx - 100, cy + 24, cx + 100, cy + 34);
    if (prog_shown < 0) { FillRect(&b, &qd.white); InsetRect(&b, 1, 1); FillRect(&b, &qd.black); InsetRect(&b, -1, -1); }
    SetRect(&b, cx - 98, cy + 26, cx - 98 + w, cy + 32);
    FillRect(&b, &qd.white);
    prog_shown = w;
}
static void gen_progress(short v)
{
    dbg.genProg = (short)(prog_base + (long)v * prog_span / 1000);
    progress_bar(dbg.genProg);
}

/* the message screen with an empty progress bar */
static void progress_message(const char *s1, const char *s2)
{
    message(s1, s2);
    prog_shown = -1;
    progress_bar(0);
}

/* nothing in the world but blocks: no inventory, armour, chests, furnaces, objects (torches,
   plants, doors), mobs, arrows, dropped items or torch light */
static void clear_things(void)
{
    memset(inv, 0, sizeof inv); memset(inv_arm, 0, sizeof inv_arm);
    memset(chests, 0, sizeof chests); memset(furns, 0, sizeof furns);
    memset(objs, 0, sizeof objs); obj_reindex();
    memset(mobs, 0, sizeof mobs); memset(arrows, 0, sizeof arrows);
    memset(drops, 0, sizeof drops); memset(drop_age, 0, sizeof drop_age);
    memset(bw_lit, 0, sizeof bw_lit);
    kvx = kvz = 0;
}

static void fresh_life(long sx, long sy, long sz)
{
    memset(&life, 0, sizeof life);
    life.health = 20; life.food = 20; life.tod = 1000; life.air = 300;
    life.sx = sx; life.sy = sy; life.sz = sz;
    life.sky = -1;
}

/* a new world for the seed, with the progress bar; the player at its spawn, fresh */
static void make_world(unsigned short seed)
{
    long sx, sy, sz;
    unsigned long t0 = pl_ticks();
    progress_message("Blockworld", "Making a new world...");
    clear_things();
    prog_base = 0; prog_span = 830;                                 /* (measured: the segment lists */
    world_progress = gen_progress;                                  /*  take 17 % of the time) */
    world_generate(seed);
    prog_base = 830; prog_span = 170;
    world_build_segments();
    world_progress = 0;
    progress_bar(1000);
    world_seed = seed;
    spawn(&sx, &sy, &sz);
    player_init(&pl, sx, sy, sz, open_yaw((short)(sx >> 8), (short)(sz >> 8)));
    set_aim(0);
    inv_sel = 0;
    fresh_life(sx, sy, sz);
    dbg.genTicks = pl_ticks() - t0;
}

/* load a world file: the player, life and (if saved) the view; 0 or the error */
static short load_file(const unsigned char *name, short *dmax)
{
    SaveState st;
    short err = load_world(&st, name, light_all);
    dbg.tRead = save_t[0]; dbg.tUnpack = save_t[1]; dbg.tWorld = save_t[2];
    if (err) return err;
    world_seed = st.seed;
    player_init(&pl, st.px, st.py, st.pz, st.yaw);
    set_aim(aim_of_rows(st.pitch));
    inv_sel = st.sel;
    fresh_life(st.sx, st.sy, st.sz);
    life.health = st.health; life.food = st.food; life.tod = st.time;
    life.bed = st.bed; life.bx = st.bx; life.by = st.by; life.bz = st.bz;
    if ((st.dist & 255) >= 8 && (st.dist & 255) <= 12) *dmax = (short)(st.dist & 255);
    if ((st.dist >> 8) >= 1 && (st.dist >> 8) <= 3) vsize = (short)((st.dist >> 8) - 1);   /* (+1: 0 = none saved) */
    kvx = kvz = 0;
    return 0;
}

/* save the world ("Saving the world..." on the screen) */
static void save_now(short dmax)
{
    SaveState st;
    unsigned long t0 = pl_ticks();
    message("Blockworld", "Saving the world...");
    memset(&st, 0, sizeof st);
    st.seed = world_seed;
    st.px = pl.x; st.py = pl.y; st.pz = pl.z; st.yaw = pl.yaw; st.pitch = pl.pitch;
    st.sel = inv_sel;
    st.health = life.health; st.food = life.food; st.time = life.tod;
    st.bed = life.bed; st.bx = life.bx; st.by = life.by; st.bz = life.bz;
    st.sx = life.sx; st.sy = life.sy; st.sz = life.sz;
    st.dist = (short)(dmax | ((vsize + 1) << 8));
    dbg.saveErr = save_world(&st);
    dbg.tPack = save_t[3]; dbg.tLZ = save_t[4]; dbg.tWrite = save_t[5];
    { extern short save_tries; dbg.saveTries = save_tries; }
    dbg.tSave = pl_ticks() - t0;
}

/* the menu's New world (how M_NEWWORLD; seed 0: from the clock) or Starting world (M_START, the
   file shipped with the game; a new world if it's missing), saved at once */
static unsigned short new_seed(void);
static void change_world(short how, unsigned short seed, short *dmax, short nosave)
{
    dbg.state = 1;
    if (how == M_START) {
        message("Blockworld", "Back to the starting world...");
        if (load_file(default_name, dmax)) {
            unsigned long t = pl_ticks();
            message("Blockworld", "The starting world is missing.");
            while (pl_ticks() - t < 120) ;
            how = M_NEWWORLD;
        }
    }
    if (how == M_NEWWORLD) make_world(seed ? seed : new_seed());
    if (!nosave) save_now(*dmax);
    dbg.worlds++;
    dbg.seed = world_seed;
    dbg.px = pl.x; dbg.py = pl.y; dbg.pz = pl.z; dbg.yaw = pl.yaw; dbg.pitch = pl.pitch; dbg.aim = aim;
    dbg.state = 2;
}
/* main's frame state after a world change: draw everything again */
#define NEW_WORLD_LOCALS() do { memset(&last, 0, sizeof last); last.x = -1; memset(&drawn_pk, 0, sizeof drawn_pk); \
        prog = 0; mx = -1; target = -1; atk = 0; need_refine = 0; msg_t = 0; t_prev = pl_ticks(); acc = 0; } while (0)

/* a seed for a new world: from the clock, not the current one */
static unsigned short new_seed(void)
{
    unsigned long t = pl_ticks() * 2654435761UL ^ (*(volatile unsigned long *)0x20C) * 40503UL;   /* Ticks, Time */
    unsigned short s = (unsigned short)(t >> 16 ^ t);
    if (!s || s == (unsigned short)world_seed) s = (unsigned short)(s + 7919);
    return s;
}

int main(void)
{
    GrafPort port;
    RCam last, cam, rc;
    Pick pk, drawn_pk;
    short dmax = 10, need_refine = 0, drawn_prog = 0, prog = 0;     /* Normal (Far 12 is in the menu) */
    unsigned long ref_ticks = 0;                                     /* a stepped refinement's ticks so far */
    short shown_h = -1, shown_f = -1, shown_a = -1;
    short mx = -1, my = -1, mz = -1, sprites = 0, tmoving = 1, target = -1, atk = 0;
    unsigned long t_prev, acc = 0;

    dbg.magic = MAGIC;
    dbg.notMagic = ~MAGIC;
    dbg.panel = -1;
    dbg.step = 1;
    {
        extern short *key_trace;
        key_trace = &dbg.lastKey;
    }
    MaxApplZone();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitCursor();
    HideCursor();
    FlushEvents(everyEvent, 0);
    OpenPort(&port);
    {
        Rect b = qd.screenBits.bounds;
        mouse_cx = (b.left + b.right) / 2;
        mouse_cy = (b.top + b.bottom) / 2;
        fullnc = (short)(((b.right - b.left) / 16) * 4);              /* Full: the screen's width */
        if (fullnc > VW_MAX / 4) fullnc = VW_MAX / 4;
    }
    dbg.machine = (short)(*(unsigned short *)(*(long *)0x2AE + 8) & 0xFF);   /* ROM version: 0x75 Plus, 0x76 SE */
    vsize = 2;              /* the default: Full (walking measured at 3.7 frames/s on a Plus and on an SE) */
    message("Blockworld", "Loading the world...");
    dbg.step = 10;
    buf = (unsigned char *)NewPtrClear((long)(VW_MAX / 8) * VH_MAX + 16);
    tbuf = (unsigned char *)NewPtrClear((long)(VW_MAX / 8) * VH_MAX + 16);
    dbg.step = 11;
    if (!buf || !tbuf || world_init(mac_alloc)) {                  /* (System 7 / MultiFinder: a
                                                                      partition too small) */
        dbg.state = 0xDEAD;
        message("Blockworld needs 2.5 MB of memory.", "Click to quit.");
        while (!Button()) ;
        ExitToShell();
    }
    dbg.buf = (unsigned long)buf;
    dbg.step = 2;
    dbg.state = 1;
    t_prev = pl_ticks();
    /* the player's world (or a save that was written but not yet renamed); else the starting world
       shipped with the game; else a new one */
    if (!(dbg.loadErr = load_file(save_name, &dmax)) || !load_file(temp_name, &dmax)) dbg.loaded = 1;
    else if (!(dbg.defErr = load_file(default_name, &dmax))) dbg.loaded = 2;
    else make_world(SEED);
    dbg.seed = world_seed;
    dbg.step = 3;
    dbg.loadTicks = pl_ticks() - t_prev;
    dbg.freeMem = FreeMem();
    dbg.dmax = dmax;
    snd_init();
    dbg.step = 4;
    {
        extern struct SndTask snd_task;
        extern short snd_installed;
        dbg.sndAddr = (unsigned long)&snd_task; dbg.sndInst = (unsigned long)&snd_installed;
    }
    set_view(vsize, dmax);
    dbg.step = 5;
    dbg.invAddr = (unsigned long)inv; dbg.armAddr = (unsigned long)inv_arm;
    dbg.chestsAddr = (unsigned long)chests; dbg.furnsAddr = (unsigned long)furns;
    dbg.objsAddr = (unsigned long)objs;
    dbg.mobsAddr = (unsigned long)mobs; dbg.arrowsAddr = (unsigned long)arrows;
    dbg.dropsAddr = (unsigned long)drops;
    dbg.mainAddr = (unsigned long)main;
    dbg.blocksAddr = (unsigned long)bw_blocks;
    mw.hurt_player = mw_hurt; mw.explode = explode; mw.drop = mw_drop;
    memset(&last, 0, sizeof last);
    memset(&drawn_pk, 0, sizeof drawn_pk);
    last.x = -1;
    {   /* the cursor's start position is not a movement */
        short dx, dy, num;
        read_input(&dx, &dy, &num);
    }
    dbg.state = 2;
    t_prev = pl_ticks();
    for (;;) {
        short in, mdx, mdy, num, steps, redraw = 0, hud = 0;
        unsigned long now = pl_ticks(), t0;
        dbg.ticks = now;
        in = read_input(&mdx, &mdy, &num);
        dbg.keys = in; dbg.mdx = mdx; dbg.mdy = mdy;
        if (in & IN_QUIT) {                                          /* Esc: the menu */
            short d = dmax;
            dbg.panel = 9;
            hud_message(0); msg_t = 0;
            short sz = vsize, res = menu_run(PANEL_X, PANEL_Y, &d, &sz, (short)(fullnc * 4));
            if (res == M_QUIT) break;
            dbg.panel = -1;
            if (res == M_NEWWORLD || res == M_START) {                /* (confirmed in the menu) */
                change_world(res, 0, &d, 0);
                sz = (short)(vsize | 0x100);                          /* (everything drawn again below) */
                NEW_WORLD_LOCALS();
            }
            if (sz != vsize) { dmax = d; set_view((short)(sz & 3), dmax); shown_h = shown_f = shown_a = -1; hud_message(0); }
            else if (d != dmax) { dmax = d; r_config(NC, VH, dmax); set_sky(life.sky); dbg.dmax = dmax; }
            read_input(&mdx, &mdy, &num);                            /* (the menu moved the cursor: */
            mdx = mdy = 0; num = -1;                                 /*  not a look) */
            t_prev = pl_ticks();
            redraw = 1;
        }
        if (in & IN_VIEW) {                                          /* view distance 12 -> 8 -> 10 */
            dmax = dmax == 12 ? 8 : dmax == 8 ? 10 : 12;
            r_config(NC, VH, dmax);
            dbg.dmax = dmax;
            redraw = 1;
        }
        if (num >= 0 && num != inv_sel) { inv_sel = num; hud = 1; }
        /* the mouse turns and looks at once; movement and mining in 20 Hz steps */
        if (mdx || mdy) {
            player_look(&pl, (short)(mdx * SENS), 0);
            set_aim((short)(aim - mdy * SENS));                      /* (as yaw: 1 pixel, 0.7 degrees) */
        }
        acc += (now - t_prev) * 20;
        t_prev = now;
        steps = (short)(acc / 60);
        acc -= (unsigned long)steps * 60;
        if (steps > 8) steps = 8;
        while (steps-- > 0) {
            short fwd = (short)(((in & IN_FWD) != 0) - ((in & IN_BACK) != 0));
            short str = (short)(((in & IN_RIGHT) != 0) - ((in & IN_LEFT) != 0));
            short dy = (short)((in & IN_TURNR ? TURN : 0) - (in & IN_TURNL ? TURN : 0));
            short dp = (short)((in & IN_LOOKU ? LOOK : 0) - (in & IN_LOOKD ? LOOK : 0));
            short hard;
            player_look(&pl, dy, 0);
            set_aim((short)(aim + dp));                              /* (player_look clamps the pitch) */
            player_tick(&pl, fwd, str, (short)((in & IN_JUMP) != 0));
            /* (the next step takes PL_GRAV off these speeds) */
            if (block_at(pl.x, pl.y + 128, pl.z) == WATER || block_at(pl.x, pl.y + 128, pl.z) == LAVA) {
                if (in & IN_JUMP)                                     /* swimming: up 3 blocks/s */
                    pl.vy = block_at(pl.x, pl.y + PL_EYE, pl.z) == WATER ? 60 : (pl.vy < 60 ? PL_JUMP : pl.vy);
                else if (pl.vy < -4) pl.vy = -4;                      /* sinking: 2 blocks/s */
            } else if (block_at(pl.x, pl.y, pl.z) == LADDER || block_at(pl.x, pl.y + 256, pl.z) == LADDER) {
                if (fwd > 0 || (in & IN_JUMP)) pl.vy = 44;               /* climbing: 2 blocks/s */
                else if (pl.vy < 8) pl.vy = 8;                        /* sliding down: 1 block/s */
            }
            if (kvx || kvz) {                                        /* knocked back */
                player_push(&pl, kvx, kvz);
                kvx = (short)(kvx * 3 / 4); kvz = (short)(kvz * 3 / 4);
            }
            dbg.steps++;
            mw.night = NIGHT(life.tod);
            mw.spawn = !dbg.spawnOff;
            if (mobs_tick(&pl, &mw)) sprites = 1;
            {
                short dt = drops_tick(&pl, dbg.collectAll);
                if (dt) sprites = 1;
                if (dt & 2) hud = 1;                                 /* (not while they only move: */
            }                                                        /*  redrawing the hotbar flickers) */
            /* the crosshair on a mob nearer than the block: a hit every half second */
            set_cam(&cam);
            pick_ray(&cam, aim, &pk, pick_solid);
            target = -1;
            if (in & IN_MINE) {
                long md = 0;
                if (aim <= AIM_R && aim >= -AIM_R) target = mob_pick(&cam, VW, VH, 3 * 256 + 128, &md);
                if (target >= 0 && pk.hit) {
                    long c = bw_sin[(cam.yaw + 256) & 1023], s = bw_sin[cam.yaw & 1023];
                    long bd = ((((long)pk.x << 8) + 128 - cam.x) * c + (((long)pk.z << 8) + 128 - cam.z) * s) >> 14;
                    if (bd < md) target = -1;
                }
            }
            if (atk) atk--;
            /* mining: hold the button on the same block for its hardness in steps */
            hard = !pk.hit ? 0 : bw_blocks[BIDX(pk.x, pk.z, pk.y)] == AIR ? 1     /* an object */
                   : mine_time(bw_blocks[BIDX(pk.x, pk.z, pk.y)], held_item());
            if ((in & IN_MINE) && target >= 0) {
                if (!atk) {
                    short dmg = attack_damage();
                    if (!pl.ground && pl.vy < 0) dmg = (short)(dmg * 3 / 2);    /* falling: a critical hit */
                    mob_hurt(target, dmg, pl.yaw, &mw);
                    if (IS_TOOL(held_item())) inv_use_tool();
                    atk = 10;
                    sprites = hud = 1;
                }
                prog = 0; mx = -1;
            } else if ((in & IN_MINE) && hard) {
                if (pk.x != mx || pk.y != my || pk.z != mz) { mx = pk.x; my = pk.y; mz = pk.z; prog = 0; }
                if (!(prog & 3)) snd_play(FX_DIG);
                if (++prog >= hard) {
                    snd_play(FX_BREAK);
                    short b = bw_blocks[BIDX(pk.x, pk.z, pk.y)], n;
                    short drop = mine_drop(b, held_item(), &n);
                    if (b == AIR) {                                  /* an object */
                        short oi = obj_at(pk.x, pk.y, pk.z);
                        drop = oi >= 0 ? obj_drop(oi, &n) : 0;
                        if (oi >= 0 && objs[oi].type == O_PLANT && objs[oi].state >= 23) drop_at(I_SEEDS, (short)(1 + (rnd() & 1)), pk.x, pk.y, pk.z);
                        if (oi >= 0) remove_obj(oi);
                    } else {
                        if (b == CHEST || b == FURNACE) container_broken(pk.x, pk.y, pk.z);
                        bw_blocks[BIDX(pk.x, pk.z, pk.y)] = AIR;
                        world_column_changed(pk.x, pk.z);
                        support_gone(pk.x, pk.y, pk.z);
                        settle(pk.x, pk.z);
                    }
                    if (b == GRAVEL && rnd() % 10 == 0) drop = I_FLINT;
                    if (drop) drop_at(drop, n, pk.x, pk.y, pk.z);
                    if (b == TURF && rnd() % 8 == 0) drop_at(I_SEEDS, 1, pk.x, pk.y, pk.z);
                    if (b == LEAVES && rnd() % 20 == 0) drop_at(I_SAPLING, 1, pk.x, pk.y, pk.z);
                    if (b == LEAVES && rnd() % 100 == 0) drop_at(I_APPLE, 1, pk.x, pk.y, pk.z);
                    if (IS_TOOL(held_item())) inv_use_tool();
                    dbg.mined++;
                    hud = redraw = 1;
                    prog = 0; mx = -1;
                }
            } else {
                prog = 0; mx = -1;
            }
            furnaces_tick();
            dbg.furnTicks++;
            if (plants_tick()) redraw = 1;
            life_tick((short)(fwd || str));
        }
        if (life.health <= 0) { die(); hud = redraw = 1; }
        if (sky_phase(life.tod) != life.sky) { set_sky(sky_phase(life.tod)); redraw = 1; }
        if (life.health != shown_h || life.food != shown_f || life.air / 15 != shown_a) {
            shown_h = life.health; shown_f = life.food; shown_a = (short)(life.air / 15);
            hud_status(shown_h < 0 ? 0 : shown_h, shown_f, shown_a > 20 ? 20 : shown_a);
        }
        set_cam(&cam);
        pick_ray(&cam, aim, &pk, pick_solid);
        if (dbg.cmd == 1) { inv_add((unsigned char)dbg.arg[0], dbg.arg[1]); dbg.cmd = 0; hud = 1; }
        if (dbg.cmd == 3) { life.tod = (long)dbg.arg[0] * 10; dbg.cmd = 0; }
        if (dbg.cmd == 5) { pl.y = (long)dbg.arg[0] << 8; pl.vy = 0; pl.ground = 0; dbg.cmd = 0; }
        if (dbg.cmd == 4) { life.health = dbg.arg[0]; life.food = dbg.arg[1]; dbg.cmd = 0; }
        if (dbg.cmd == 6) {                                          /* tests: a mob arg1 blocks ahead */
            long c = bw_sin[(pl.yaw + 256) & 1023], s = bw_sin[pl.yaw & 1023];
            long x = pl.x + ((c * dbg.arg[1]) >> 6), z = pl.z + ((s * dbg.arg[1]) >> 6), y = pl.y + 64;
            short t = top_of((short)(x >> 8), (short)(z >> 8)), i;
            if (t < WY && labs(((long)t << 8) - pl.y) < 4 * 256) y = (long)t << 8;   /* on the ground there */
            i = mob_spawn(dbg.arg[0], x, y, z);
            if (i >= 0) { mobs[i].yaw = dbg.arg[2]; mobs[i].timer = 30000; }  /* (an animal stands until hit) */
            dbg.cmd = 0; sprites = 1;
        }
        if (dbg.cmd == 8) {                                          /* tests: block arg0 >> 8 at (arg0 & 255, arg2, arg1) */
            short bx = (short)(dbg.arg[0] & 255), bz = dbg.arg[1], by = dbg.arg[2];
            if (bx < WX && bz >= 0 && bz < WX && by >= 0 && by < WY) {
                bw_blocks[BIDX(bx, bz, by)] = (unsigned char)(dbg.arg[0] >> 8);
                world_column_changed(bx, bz);
                settle(bx, bz);
            }
            dbg.cmd = 0; redraw = 1;
        }
        if (dbg.cmd == 9) {                                          /* tests: pitch arg0 rows (and yaw arg1 if arg2) */
            set_aim(dbg.arg[0] < -PITCH_R ? (short)-(AIM_R + (-dbg.arg[0] - PITCH_R) / 4)
                    : dbg.arg[0] > PITCH_R ? (short)(AIM_R + (dbg.arg[0] - PITCH_R) / 4) : aim_of_rows(dbg.arg[0]));
            if (dbg.arg[2]) pl.yaw = (short)(dbg.arg[1] & 1023);
            dbg.cmd = 0; redraw = 1;
            set_cam(&cam);
            pick_ray(&cam, aim, &pk, pick_solid);
        }
        if (dbg.cmd == 10) {                                         /* tests: view size arg0 (distance arg1 if set) */
            if (dbg.arg[1] >= 8 && dbg.arg[1] <= 12) dmax = dbg.arg[1];
            set_view((short)(dbg.arg[0] % 3), dmax);
            shown_h = shown_f = shown_a = -1;
            dbg.cmd = 0; redraw = 1;
        }
        if (dbg.cmd == 11 || dbg.cmd == 12) {                        /* tests: a new world (seed arg0) / the starting world; */
            change_world(dbg.cmd == 11 ? M_NEWWORLD : M_START, (unsigned short)dbg.arg[0], &dmax, dbg.arg[1]);   /* arg1: no save */
            set_view(vsize, dmax);
            shown_h = shown_f = shown_a = -1;
            hud_message(0);
            NEW_WORLD_LOCALS();
            set_cam(&cam);
            pick_ray(&cam, aim, &pk, pick_solid);
            dbg.cmd = 0; redraw = hud = 1;
        }
        if (dbg.cmd == 16) {                                         /* tests: a 64 KB buffer for command 14 */
            extern unsigned char *save_test_buf;
            if (!save_test_buf) save_test_buf = (unsigned char *)NewPtr(65536L);
            dbg.testBuf = (unsigned long)save_test_buf;
            dbg.cmd = 0;
        }
        if (dbg.cmd == 14) {                                         /* tests: write "Blockworld Test" (arg0 KB in pieces of
                                                                        arg1 KB; arg2 the bytes: save.c save_test) */
            extern short save_test(long n, long piece, short heads);
            dbg.saveErr = save_test((long)dbg.arg[0] * 1024, (long)(dbg.arg[1] ? dbg.arg[1] : 64) * 1024, dbg.arg[2]);
            dbg.cmd = 0;
        }
        if (dbg.cmd == 7) { memset(mobs, 0, sizeof mobs); memset(arrows, 0, sizeof arrows); dbg.cmd = 0; sprites = 1; }
        dbg.target = target; dbg.atkCool = atk; dbg.kills = mw.kills;
        if (dbg.cmd == 2) {                                          /* tests: stand at (arg0, arg1), yaw arg2 */
            player_init(&pl, ((long)dbg.arg[0] << 8) + 128, (long)top_of(dbg.arg[0], dbg.arg[1]) << 8,
                        ((long)dbg.arg[1] << 8) + 128, dbg.arg[2]);
            set_aim(0);
            dbg.cmd = 0;
            set_cam(&cam);
            pick_ray(&cam, aim, &pk, pick_solid);
        }
        /* E with the bow: an arrow along the view */
        if ((in & IN_PLACE) && held_item() == I_BOW) {
            if (take_one(I_ARROW)) {
                long c = bw_sin[(pl.yaw + 256) & 1023], s = bw_sin[pl.yaw & 1023];
                long ca = bw_sin[(aim + 256) & 1023];
                c = (c * ca) >> 14; s = (s * ca) >> 14;
                arrow_shoot(pl.x + ((c * 80) >> 14), pl.y + PL_EYE - 16, pl.z + ((s * 80) >> 14),
                            (short)((c * 200) >> 14), (short)((bw_sin[aim & 1023] * 200L) >> 14), (short)((s * 200) >> 14), 0);
                inv_use_tool();
                snd_play(FX_BOW);
                hud = sprites = 1;
            } else
                say("No arrows.");
            in = (short)(in & ~IN_PLACE);
        }
        /* E: use a workbench, chest or furnace; else put the selected block in front of the face */
        if ((in & IN_PLACE) && pk.hit) {
            short b = bw_blocks[BIDX(pk.x, pk.z, pk.y)], it = held_item();
            short panel = (in & IN_SHIFT) ? -1 : b == BENCH ? P_BENCH : b == CHEST ? P_CHEST : b == FURNACE ? P_FURNACE : -1;
            if (panel >= 0) {
                void *c = panel == P_CHEST ? (void *)chest_at(pk.x, pk.y, pk.z, 1)
                        : panel == P_FURNACE ? (void *)furn_at(pk.x, pk.y, pk.z, 1) : 0;
                if (panel == P_BENCH || c) {
                    dbg.panel = panel;
                    hud_message(0); msg_t = 0;
                    panel_run(panel, c, PANEL_X, PANEL_Y, VW, VH, furn_poll);
                    dbg.panel = -1;
                    read_input(&mdx, &mdy, &num);                    /* (the cursor moved meanwhile) */
                    hud = redraw = 1;
                }
            } else if (b == BED && !(in & IN_SHIFT)) {               /* a bed: respawn here; sleep at night */
                life.bed = 1; life.bx = pk.x; life.by = (short)(pk.y + 1); life.bz = pk.z;
                if (NIGHT(life.tod) && mobs_hostile_near(pl.x, pl.z, 8L * 256))
                    say("You can't sleep now: monsters are near.");
                else if (NIGHT(life.tod)) { life.tod = 0; say("You sleep until morning."); }
                else say("Your bed is set. You can only sleep at night.");
                redraw = 1;
            } else if (item_food[it] && life.food < 20) {             /* eat */
                life.food = (short)(life.food + item_food[it]);
                snd_play(FX_EAT);
                if (life.food > 20) life.food = 20;
                if (!--inv[inv_sel].count) inv[inv_sel].item = 0;
                hud = 1;
            } else if (b == DOOR) {                                  /* open a door: it becomes passable */
                bw_blocks[BIDX(pk.x, pk.z, pk.y)] = AIR;
                world_column_changed(pk.x, pk.z);
                obj_add(pk.x, pk.y, pk.z, O_DOOR, 0);
                redraw = 1;
            } else if (IS_TOOL(it) && TOOL_KIND(it) == K_HOE && pk.face == F_YP && (b == TURF || b == SOIL)
                       && obj_at(pk.x, (short)(pk.y + 1), pk.z) < 0) {
                bw_blocks[BIDX(pk.x, pk.z, pk.y)] = FARMLAND;      /* till the ground */
                world_column_changed(pk.x, pk.z);
                inv_use_tool();
                hud = redraw = 1;
            } else if ((it == I_TORCH || (it == I_SAPLING && pk.face == F_YP && (b == TURF || b == SOIL))
                        || (it == I_SEEDS && pk.face == F_YP && b == FARMLAND))
                       && pk.ax >= 0 && pk.ax < WX && pk.az >= 0 && pk.az < WX && pk.ay >= 0 && pk.ay < WY
                       && bw_blocks[BIDX(pk.ax, pk.az, pk.ay)] == AIR && obj_at(pk.ax, pk.ay, pk.az) < 0) {
                if (obj_add(pk.ax, pk.ay, pk.az, it == I_TORCH ? O_TORCH : O_PLANT, (unsigned char)(it == I_SEEDS ? 16 : 0)) >= 0) {
                    if (it == I_TORCH) relight(pk.ax, pk.az);
                    if (!--inv[inv_sel].count) inv[inv_sel].item = 0;
                    dbg.placed++;
                    hud = redraw = 1;
                }
            } else if (it > 0 && it < NBLOCKS && it != BEDROCK && it != WATER && it != LAVA && it != FARMLAND
                && pk.ax >= 0 && pk.ax < WX && pk.az >= 0 && pk.az < WX && pk.ay >= 0 && pk.ay < WY
                && (bw_blocks[BIDX(pk.ax, pk.az, pk.ay)] == AIR || bw_blocks[BIDX(pk.ax, pk.az, pk.ay)] == WATER)
                && obj_at(pk.ax, pk.ay, pk.az) < 0 && !player_in(pk.ax, pk.ay, pk.az)) {
                bw_blocks[BIDX(pk.ax, pk.az, pk.ay)] = (unsigned char)it;
                world_column_changed(pk.ax, pk.az);
                snd_play(FX_PLACE);
                if (it == SAND || it == GRAVEL) settle(pk.ax, pk.az);
                if (!--inv[inv_sel].count) inv[inv_sel].item = 0;
                dbg.placed++;
                hud = redraw = 1;
                pick_ray(&cam, aim, &pk, pick_solid);
            }
        }
        if (in & IN_INV) {                                           /* Tab: the inventory */
            dbg.panel = P_INV;
            hud_message(0); msg_t = 0;
            panel_run(P_INV, 0, PANEL_X, PANEL_Y, VW, VH, furn_poll);
            dbg.panel = -1;
            read_input(&mdx, &mdy, &num);
            hud = redraw = 1;
        }
        if (hud) hud_hotbar();
        dbg.px = pl.x; dbg.py = pl.y; dbg.pz = pl.z; dbg.yaw = pl.yaw; dbg.pitch = pl.pitch;
        dbg.autojumps = (short)pl.autojumps; dbg.ground = pl.ground;
        dbg.hit = pk.hit; dbg.hx = pk.x; dbg.hy = pk.y; dbg.hz = pk.z; dbg.face = pk.face; dbg.prog = prog;
        memcpy(dbg.hot, inv, sizeof dbg.hot);
        dbg.sel = inv_sel;
        dbg.health = life.health; dbg.food = life.food; dbg.air = life.air; dbg.tod = life.tod;
        t0 = pl_ticks();
        if (dbg.bench) redraw = 1;
        render_cam(&rc, &cam, &pk);
        dbg.aim = aim;
        if (redraw || rc.x != last.x || rc.y != last.y || rc.z != last.z || rc.yaw != last.yaw
            || rc.pitch != last.pitch) {
            tmoving = vsize == 2 ? 2 : 1;
            if (tmoving == 2) {                                     /* Full: every 4th column, 16 px */
                r_cast_cols_asm(&rc, 0, 4);
                r_raster16_asm(buf);
            } else {
                r_cast_cols_asm(&rc, 0, 2);                         /* moving: even columns, 8 px */
                r_raster8_asm(buf, VW / 8);
            }
            sky_draw(&rc, tmoving);
            memcpy(tbuf, buf, (VW / 8) * VH);
            objs_draw(&rc, buf, VW, VH, tmoving, dmax);
            drops_draw(&rc, buf, VW, VH, tmoving, dmax);
            mobs_draw(&rc, buf, VW, VH, tmoving, dmax);
            sprites = 0;
            drawn_pk = pk;
            drawn_prog = bar_len(&pk, prog);
            overlays(&rc, &cam, &pk, drawn_prog);
            r_present(view.base, view.rowBytes, buf);
            last = rc;
            need_refine = 1;
            dbg.moving++;
            dbg.lastFrame = pl_ticks() - t0;
            dbg.moveTicks += dbg.lastFrame;
            dbg.moveCount++;
            dbg.frames++;
        } else if (need_refine == 1 && tmoving == 2) {
            /* Full, standing still: the refinement in steps, the input read between them (a whole
               one takes about 0.7 s on a Plus): first every even column, 8 px wide, shown */
            r_cast_cols_asm(&rc, 2, 4);
            r_raster8_asm(buf, VW / 8);
            sky_draw(&rc, 1);
            memcpy(tbuf, buf, (VW / 8) * VH);
            tmoving = 1;
            objs_draw(&rc, buf, VW, VH, 1, dmax);
            drops_draw(&rc, buf, VW, VH, 1, dmax);
            mobs_draw(&rc, buf, VW, VH, 1, dmax);
            sprites = 0;
            drawn_pk = pk;
            drawn_prog = bar_len(&pk, prog);
            overlays(&rc, &cam, &pk, drawn_prog);
            r_present(view.base, view.rowBytes, buf);
            need_refine = 2;
            ref_ticks = pl_ticks() - t0;
            dbg.frames++;
        } else if (need_refine == 2) {                              /* then columns 1, 5, 9, ... */
            r_cast_cols_asm(&rc, 1, 4);
            need_refine = 3;
            ref_ticks += pl_ticks() - t0;
        } else if (need_refine) {
            if (need_refine == 3) r_cast_cols_asm(&rc, 3, 4);       /* (Full: the last quarter) */
            else { r_cast_cols_asm(&rc, 1, 2); ref_ticks = 0; }     /* still: the odd columns too */
            r_edges_asm();
            r_raster_asm(buf, VW / 8);
            sky_draw(&rc, 0);
            memcpy(tbuf, buf, (VW / 8) * VH);
            tmoving = 0;
            objs_draw(&rc, buf, VW, VH, 0, dmax);
            drops_draw(&rc, buf, VW, VH, 0, dmax);
            mobs_draw(&rc, buf, VW, VH, 0, dmax);
            sprites = 0;
            drawn_pk = pk;
            drawn_prog = bar_len(&pk, prog);
            overlays(&rc, &cam, &pk, drawn_prog);
            r_present(view.base, view.rowBytes, buf);
            need_refine = 0;
            dbg.refines++;
            dbg.refineTicks = ref_ticks + (pl_ticks() - t0);        /* (all its steps) */
            dbg.frames++;
        } else if (sprites || bar_len(&pk, prog) != drawn_prog || pk.hit != drawn_pk.hit
                   || (pk.hit && (pk.x != drawn_pk.x || pk.y != drawn_pk.y || pk.z != drawn_pk.z || pk.face != drawn_pk.face))) {
            /* the same view; mobs, the outline or the bar changed: all of them again over the terrain */
            memcpy(buf, tbuf, (VW / 8) * VH);
            objs_draw(&rc, buf, VW, VH, tmoving, dmax);
            drops_draw(&rc, buf, VW, VH, tmoving, dmax);
            mobs_draw(&rc, buf, VW, VH, tmoving, dmax);
            drawn_pk = pk;
            drawn_prog = bar_len(&pk, prog);
            overlays(&rc, &cam, &pk, drawn_prog);
            r_present(view.base, view.rowBytes, buf);
            sprites = 0;
            dbg.mobFrames++;
        }
    }
    save_now(dmax);
    snd_quit();
    ExitToShell();
    return 0;
}

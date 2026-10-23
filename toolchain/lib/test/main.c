/*
 * Test for toolchain/lib (plus.h). Results go into a 'PLIB' block read by check_lib.lua:
 *   1. pl_view_init + pl_present of a known pattern; waits until the Lua has compared the
 *      screen with the buffer byte for byte and set blk.ack (or 60 s),
 *   2. speed: 60 x pl_present versus 60 x CopyBits of the same 384x288 buffer,
 *   3. pacing: 60 frames at 30 Hz idle, then 60 frames each doing a present,
 *   4. input loop at 30 Hz: pl_input_update each frame (the Lua presses keys); the arrows move
 *      a 16x16 square, X/Z/Return/Tab/Space toggle markers. Runs until the machine stops.
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
#include "plus.h"

#ifndef W
#define W 384
#endif
#define H 288
#define RB (W / 8)
#define MAGIC 0x504C4942UL    /* 'PLIB' */

struct Block {
    unsigned long magic, version, notMagic;   /*  0 */
    unsigned long state;                      /* 12: 1 pattern shown, 2 speed done, 3 pacing done, 4 input loop */
    unsigned long passBits, nChecks;          /* 16, 20: checks done inside the app */
    unsigned long buf;                        /* 24: offscreen buffer address */
    unsigned long viewBase;                   /* 28: screen address of the view */
    Rect viewRect;                            /* 32: view, global coordinates */
    short srcRowBytes, w, h, screenRowBytes;  /* 40..47 */
    unsigned long ticksPresent60;             /* 48: 60 x pl_present */
    unsigned long ticksCopyBits60;            /* 52: 60 x CopyBits, same buffer and place */
    unsigned long paceIdle, paceIdleLate;     /* 56, 60: ticks for 60 frames at period 2; late frames */
    unsigned long paceLoad, paceLoadLate;     /* 64, 68: same, presenting every frame */
    unsigned long held, everHeld;             /* 72, 76: PlInput.held now; OR of all held */
    unsigned long presses, frames;            /* 80, 84 */
    short squareX, squareY;                   /* 88, 90 */
    volatile unsigned long ack;               /* 92: the Lua sets it after checking the pattern */
};
static struct Block blk;

static unsigned char *buf;

static void fill_pattern(void)
{
    short x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < RB; x++)
            buf[(long)y * RB + x] = (unsigned char)(y * 7 + x * 13 + (y >> 3));
}

static void square(short x, short y, short on)   /* 16x16, x multiple of 8 */
{
    short r;
    for (r = 0; r < 16; r++) {
        unsigned char *p = buf + (long)(y + r) * RB + (x >> 3);
        p[0] = p[1] = on ? 0xFF : 0x00;
    }
}

int main(void)
{
    PlView view;
    PlInput in = {0, 0, 0};
    WindowPtr win;
    BitMap src;
    Rect r, srcRect;
    unsigned long t;
    short i;
    EventRecord ev;

    blk.magic = MAGIC; blk.version = 1; blk.notMagic = ~MAGIC;
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

    r = qd.screenBits.bounds;
    win = NewWindow(NULL, &r, "\p", true, plainDBox, (WindowPtr)-1, false, 0);
    SetPort(win);
    BeginUpdate(win);     /* the window is white already; swallow its first update */
    EndUpdate(win);

    blk.nChecks = 3;
    buf = (unsigned char *)NewPtrClear((long)RB * H);
    if (buf && pl_view_init(&view, W, H) == 0)
        blk.passBits |= 1;
    blk.buf = (unsigned long)buf;
    blk.viewBase = (unsigned long)view.base;
    blk.viewRect = view.rect;
    blk.srcRowBytes = RB; blk.w = W; blk.h = H; blk.screenRowBytes = view.rowBytes;

    /* 1. pattern */
    fill_pattern();
    pl_present(&view, buf, RB);
    blk.state = 1;
    t = pl_ticks();
    while (!blk.ack && pl_ticks() - t < 3600)   /* until the Lua has compared (at most 60 s) */
        ;

    /* 2. speed */
    t = pl_ticks();
    for (i = 0; i < 60; i++)
        pl_present(&view, buf, RB);
    blk.ticksPresent60 = pl_ticks() - t;
    src.baseAddr = (Ptr)buf;
    src.rowBytes = RB;
    SetRect(&src.bounds, 0, 0, W, H);
    srcRect = src.bounds;
    t = pl_ticks();
    for (i = 0; i < 60; i++)
        CopyBits(&src, &win->portBits, &srcRect, &view.rect, srcCopy, NULL);
    blk.ticksCopyBits60 = pl_ticks() - t;
    blk.state = 2;

    /* 3. pacing at 30 Hz */
    pl_pace_init(2);
    t = pl_ticks();
    for (i = 0; i < 60; i++)
        if (pl_pace_wait()) blk.paceIdleLate++;
    blk.paceIdle = pl_ticks() - t;
    for (i = 0; i < RB * H; i++) buf[i] = 0;
    pl_pace_init(2);
    t = pl_ticks();
    for (i = 0; i < 60; i++) {
        if (pl_pace_wait()) blk.paceLoadLate++;
        pl_present(&view, buf, RB);
    }
    blk.paceLoad = pl_ticks() - t;
    if (blk.paceIdle >= 119 && blk.paceIdle <= 121 && blk.paceIdleLate == 0)
        blk.passBits |= 2;
    if (blk.paceLoadLate == 0)
        blk.passBits |= 4;
    blk.state = 3;

    /* 4. input loop */
    blk.squareX = W / 2 - 8; blk.squareY = H / 2 - 8;
    pl_pace_init(2);
    blk.state = 4;
    for (;;) {
        pl_pace_wait();
        pl_input_update(&in);
        blk.held = in.held;
        blk.everHeld |= in.held;
        if (in.pressed) blk.presses++;
        square(blk.squareX, blk.squareY, 0);
        if (in.held & PL_LEFT  && blk.squareX >= 8)          blk.squareX -= 8;
        if (in.held & PL_RIGHT && blk.squareX <= W - 16 - 8) blk.squareX += 8;
        if (in.held & PL_UP    && blk.squareY >= 4)          blk.squareY -= 4;
        if (in.held & PL_DOWN  && blk.squareY <= H - 16 - 4) blk.squareY += 4;
        square(blk.squareX, blk.squareY, 1);
        for (i = 0; i < 11; i++)                         /* one marker per input bit, top row */
            square(16 + i * 24, 8, (in.held >> i) & 1);
        pl_present(&view, buf, RB);
        blk.frames++;
#ifdef FLUSH_KEYS
        FlushEvents(keyDownMask | keyUpMask | autoKeyMask, 0);
#endif
        if ((blk.frames & 15) == 0) {                    /* let the system breathe */
            SystemTask();
            GetNextEvent(0, &ev);
        }
    }
    return 0;
}

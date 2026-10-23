/*
 * plus.h: small shared helpers for games on the Mac Plus / SE (68000, 1-bit screen).
 *
 *   Screen:  a view of w x h pixels centred in qd.screenBits.bounds (Narrow Screen safe),
 *            and a fast copy of an offscreen 1-bit buffer into it.
 *   Input:   GetKeys mapped to the standard keys of DECISIONS.md (Plus and ADB keyboards).
 *   Pacing:  frame timing from the Ticks counter (60.15 Hz), no traps in the loop.
 *
 * C (works from C++ too). See toolchain/lib/README.md.
 */
#ifndef PLUS_H
#define PLUS_H

#include <Quickdraw.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Screen -------------------------------------------------------------------------- */

typedef struct PlView {
    unsigned char *base;   /* screen address of the view's top-left pixel */
    short rowBytes;        /* bytes per screen row (64 on the Plus and SE) */
    short w, h;            /* view size in pixels */
    Rect rect;             /* the view in global (screenBits) coordinates */
} PlView;

/* Centre a w x h view in qd.screenBits.bounds. w must be a multiple of 16. The left edge is
   rounded down to a multiple of 16 pixels (so the copy can use word/long moves, which the
   68000 only allows at even addresses). Call after InitGraf. Returns 0, or -1 if it can't fit. */
short pl_view_init(PlView *v, short w, short h);

/* Copy an offscreen 1-bit buffer (w x h pixels, srcRowBytes bytes per row, even, at least
   w/8) to the view. Writes the screen directly: hide the cursor first (HideCursor), or the
   cursor will leave droppings. pl_present_rows copies only rows y0 <= y < y1. */
void pl_present(const PlView *v, const void *src, short srcRowBytes);
void pl_present_rows(const PlView *v, const void *src, short srcRowBytes, short y0, short y1);

/* ---- Input --------------------------------------------------------------------------- */

enum {
    PL_UP = 0x0001, PL_DOWN = 0x0002, PL_LEFT = 0x0004, PL_RIGHT = 0x0008,   /* arrow keys */
    PL_A = 0x0010,       /* X: A / jump / accelerate */
    PL_B = 0x0020,       /* Z: B / run / brake */
    PL_START = 0x0040,   /* Return */
    PL_SELECT = 0x0080,  /* Tab */
    PL_SPECIAL = 0x0100, /* Space */
    PL_QUIT = 0x0200,    /* Command-Q */
    PL_ESC = 0x0400      /* Esc (ADB keyboards; the Plus keyboard has none) */
};

typedef struct PlInput {
    unsigned short held;      /* keys down now */
    unsigned short pressed;   /* went down since the previous pl_input_update */
    unsigned short released;  /* went up since the previous pl_input_update */
} PlInput;

unsigned short pl_input_read(void);   /* current state (one GetKeys call) */
void pl_input_update(PlInput *in);    /* call once per frame */

/* ---- Frame pacing -------------------------------------------------------------------- */

/* Ticks (low-memory $16A, incremented at each vertical blank, 60.15 Hz). */
#define pl_ticks() (*(volatile unsigned long *)0x16A)

/* Start pacing at one frame per `period` ticks (1 = 60 Hz, 2 = 30 Hz, 3 = 20 Hz). */
void pl_pace_init(short period);
/* Wait for the start of the next frame (busy-waits on Ticks). Returns 0 if on time, otherwise
   how many ticks late the call came; the schedule then restarts from now. */
short pl_pace_wait(void);

#ifdef __cplusplus
}
#endif

#endif

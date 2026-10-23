/* plus.c: see plus.h. */
#include "plus.h"
#include <Events.h>

/* ---- Screen -------------------------------------------------------------------------- */

short pl_view_init(PlView *v, short w, short h)
{
    Rect b = qd.screenBits.bounds;
    short left, top;

    if ((w & 15) || w > b.right - b.left || h > b.bottom - b.top)
        return -1;
    left = b.left + ((b.right - b.left - w) / 2 & ~15);
    top = b.top + (b.bottom - b.top - h) / 2;
    v->rowBytes = qd.screenBits.rowBytes & 0x3FFF;
    v->base = (unsigned char *)qd.screenBits.baseAddr
              + (long)(top - b.top) * v->rowBytes + ((left - b.left) >> 3);
    v->w = w;
    v->h = h;
    SetRect(&v->rect, left, top, left + w, top + h);
    return 0;
}

/* One row: n longs then an optional trailing word. The long loop is unrolled 8 times
   (Duff's device), which GCC turns into runs of move.l (a0)+,(a1)+. */
static inline void copy_row(const unsigned long *s, unsigned long *d, short longs, short word)
{
    short n = (longs + 7) >> 3;
    if (longs > 0) {
        switch (longs & 7) {
        case 0: do { *d++ = *s++;
        case 7:      *d++ = *s++;
        case 6:      *d++ = *s++;
        case 5:      *d++ = *s++;
        case 4:      *d++ = *s++;
        case 3:      *d++ = *s++;
        case 2:      *d++ = *s++;
        case 1:      *d++ = *s++;
                } while (--n > 0);
        }
    }
    if (word)
        *(unsigned short *)d = *(const unsigned short *)s;
}

/* Rows that are a whole number of longs (up to 32 longs = 1024 px): jump into an unrolled run
   of 32 "move.l (a0)+,(a1)+" (20 cycles per 4 bytes) and loop with dbra. */
static void copy_rows_longs(const void *src, void *dst, short rows, short longs, short srcSkip, short dstSkip)
{
    register const void *a0 __asm__("a0") = src;
    register void *a1 __asm__("a1") = dst;
    short back = longs * 2;              /* each move.l (a0)+,(a1)+ is 2 bytes of code */
    rows--;
    __asm__ volatile(
        "lea    2f(%%pc),%%a2\n\t"
        "suba.w %[back],%%a2\n"
        "1:\tjmp   (%%a2)\n\t"
        ".rept 32\n\t"
        "move.l (%%a0)+,(%%a1)+\n\t"
        ".endr\n"
        "2:\tadda.w %[ss],%%a0\n\t"
        "adda.w %[ds],%%a1\n\t"
        "dbra   %[rows],1b"
        : "+a"(a0), "+a"(a1), [rows] "+d"(rows)
        : [back] "d"(back), [ss] "d"(srcSkip), [ds] "d"(dstSkip)
        : "a2", "memory", "cc");
}

/* Same, for rows of n longs plus one word (widths that are an odd multiple of 16 px). */
static void copy_rows_longs_word(const void *src, void *dst, short rows, short longs, short srcSkip, short dstSkip)
{
    register const void *a0 __asm__("a0") = src;
    register void *a1 __asm__("a1") = dst;
    short back = longs * 2;
    rows--;
    __asm__ volatile(
        "lea    2f(%%pc),%%a2\n\t"
        "suba.w %[back],%%a2\n"
        "1:\tjmp   (%%a2)\n\t"
        ".rept 32\n\t"
        "move.l (%%a0)+,(%%a1)+\n\t"
        ".endr\n"
        "2:\tmove.w (%%a0)+,(%%a1)+\n\t"
        "adda.w %[ss],%%a0\n\t"
        "adda.w %[ds],%%a1\n\t"
        "dbra   %[rows],1b"
        : "+a"(a0), "+a"(a1), [rows] "+d"(rows)
        : [back] "d"(back), [ss] "d"(srcSkip), [ds] "d"(dstSkip)
        : "a2", "memory", "cc");
}

void pl_present_rows(const PlView *v, const void *src, short srcRowBytes, short y0, short y1)
{
    const unsigned char *s = (const unsigned char *)src + (long)y0 * srcRowBytes;
    unsigned char *d = v->base + (long)y0 * v->rowBytes;
    short bytes = v->w >> 3;
    short longs = bytes >> 2, word = bytes & 2;
    short y;

    if (y1 > v->h) y1 = v->h;
    if (y1 <= y0) return;
    if (longs <= 32) {
        if (word)
            copy_rows_longs_word(s, d, y1 - y0, longs, srcRowBytes - bytes, v->rowBytes - bytes);
        else
            copy_rows_longs(s, d, y1 - y0, longs, srcRowBytes - bytes, v->rowBytes - bytes);
        return;
    }
    for (y = y0; y < y1; y++) {
        copy_row((const unsigned long *)s, (unsigned long *)d, longs, word);
        s += srcRowBytes;
        d += v->rowBytes;
    }
}

void pl_present(const PlView *v, const void *src, short srcRowBytes)
{
    pl_present_rows(v, src, srcRowBytes, 0, v->h);
}

/* ---- Input --------------------------------------------------------------------------- */

/* Virtual key codes. Arrow keys differ: the Plus keyboard (M0110A) sends keypad-style codes,
   ADB keyboards (SE and later) their own. Both are accepted. */
static const struct { unsigned char code; unsigned short bit; } keymap[] = {
    {0x4D, PL_UP}, {0x48, PL_DOWN}, {0x46, PL_LEFT}, {0x42, PL_RIGHT},       /* Plus */
    {0x7E, PL_UP}, {0x7D, PL_DOWN}, {0x7B, PL_LEFT}, {0x7C, PL_RIGHT},       /* ADB */
    {0x07, PL_A}, {0x06, PL_B}, {0x24, PL_START}, {0x30, PL_SELECT},
    {0x31, PL_SPECIAL}, {0x35, PL_ESC},
};

#define KEY_DOWN(km, k) (((km)[(k) >> 3] >> ((k) & 7)) & 1)

unsigned short pl_input_read(void)
{
    unsigned char km[16];
    unsigned short bits = 0;
    unsigned short i;

    GetKeys(km);
    for (i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
        if (KEY_DOWN(km, keymap[i].code))
            bits |= keymap[i].bit;
    if (KEY_DOWN(km, 0x37) && KEY_DOWN(km, 0x0C))          /* Command + Q */
        bits |= PL_QUIT;
    return bits;
}

void pl_input_update(PlInput *in)
{
    unsigned short now = pl_input_read();
    in->pressed = now & ~in->held;
    in->released = in->held & ~now;
    in->held = now;
}

/* ---- Frame pacing -------------------------------------------------------------------- */

static unsigned long next_tick;
static short pace_period = 1;

void pl_pace_init(short period)
{
    pace_period = period > 0 ? period : 1;
    next_tick = pl_ticks() + pace_period;
}

short pl_pace_wait(void)
{
    unsigned long now = pl_ticks();
    long late = (long)(now - next_tick);
    if (late > 0) {                        /* frame start already passed: resynchronise */
        next_tick = now + pace_period;
        return late > 32767 ? 32767 : (short)late;
    }
    while ((long)(pl_ticks() - next_tick) < 0)
        ;
    next_tick += pace_period;
    return 0;
}

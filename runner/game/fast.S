| Rail Runner hot loops in hand-written 68000 (called from render.c; C calling convention:
| arguments on the stack; every parameter is declared long in C, so each takes 4 bytes and a
| 16-bit value sits in the low word).
#include "params.h"

#define NLONG (PF_W / 32)

        .text

| void train_rows(unsigned short *dst, const unsigned short *src, short n,
|                 long a, long astep, long b, long bstep)
| For n rows: copy pixels [a >> 16, b >> 16] (clamped to the playfield) from src to dst, then
| a += astep, b += bstep, dst and src advance one playfield row. Middle words are copied as
| longs through a jump into an unrolled block.
        .globl  train_rows
train_rows:
        movem.l %d2-%d7/%a2-%a4/%a6,-(%sp)     | 40 bytes saved
        move.l  44(%sp),%a0
        move.l  48(%sp),%a1
        move.w  54(%sp),%d7
        move.l  56(%sp),%d0
        move.l  60(%sp),%a6                    | astep
        move.l  64(%sp),%d1
        lea     lmask(%pc),%a4                 | rmask = lmask + 32
        subq.w  #1,%d7
        bmi     9f
1:      move.l  %d0,%d4
        swap    %d4                            | x0
        move.l  %d1,%d5
        swap    %d5                            | x1
        tst.w   %d4
        bmi     20f
21:     cmp.w   #PF_W-1,%d5
        bgt     22f
23:     cmp.w   %d5,%d4
        bgt.s   8f
        moveq   #15,%d2
        and.w   %d4,%d2
        add.w   %d2,%d2
        move.w  0(%a4,%d2.w),%d2               | left mask
        moveq   #15,%d3
        and.w   %d5,%d3
        add.w   %d3,%d3
        move.w  32(%a4,%d3.w),%d3              | right mask
        lsr.w   #4,%d4                         | w0
        lsr.w   #4,%d5                         | w1
        sub.w   %d4,%d5                        | words - 1
        add.w   %d4,%d4
        lea     0(%a0,%d4.w),%a2
        lea     0(%a1,%d4.w),%a3
        subq.w  #1,%d5                         | middle words (-1: one word only)
        bmi.s   30f
        move.w  (%a2),%d6                      | first word
        move.w  (%a3)+,%d4
        eor.w   %d6,%d4
        and.w   %d2,%d4
        eor.w   %d4,(%a2)+
        lsr.w   #1,%d5                         | middle: odd word, then longs
        bcc.s   4f
        move.w  (%a3)+,(%a2)+
4:      add.w   %d5,%d5
        neg.w   %d5
        jmp     5f(%pc,%d5.w)
        .rept   NLONG
        move.l  (%a3)+,(%a2)+
        .endr
5:      move.w  (%a2),%d6                      | last word
        move.w  (%a3),%d4
        eor.w   %d6,%d4
        and.w   %d3,%d4
        eor.w   %d4,(%a2)
8:      add.l   %a6,%d0
        add.l   68(%sp),%d1
        lea     PF_RB(%a0),%a0
        lea     PF_RB(%a1),%a1
        dbra    %d7,1b
9:      movem.l (%sp)+,%d2-%d7/%a2-%a4/%a6
        rts
20:     moveq   #0,%d4
        bra     21b
22:     move.w  #PF_W-1,%d5
        bra     23b
30:     and.w   %d3,%d2                        | one word: d ^= (d ^ s) & m
        move.w  (%a2),%d6
        move.w  (%a3),%d4
        eor.w   %d6,%d4
        and.w   %d2,%d4
        eor.w   %d4,(%a2)
        bra.s   8b

| void train_tab(unsigned short *dst, const unsigned short *src, long n, const short *tab)
| Like train_rows, for rows where both span edges are fixed lines: per row a table entry
| {byte offset, left mask, right mask, middle words} (middle -1: one word with the combined
| mask in the left-mask slot; -2: nothing on this row).
        .globl  train_tab
train_tab:
        movem.l %d2-%d6/%a2-%a4,-(%sp)         | 32 bytes saved
        move.l  36(%sp),%a0
        move.l  40(%sp),%a1
        move.w  46(%sp),%d6
        move.l  48(%sp),%a4
        subq.w  #1,%d6
        bmi     9f
1:      movem.w (%a4)+,%d2-%d5
        lea     0(%a0,%d2.w),%a2
        lea     0(%a1,%d2.w),%a3
        tst.w   %d5
        bmi.s   30f
        move.w  (%a2),%d0                      | first word
        move.w  (%a3)+,%d1
        eor.w   %d0,%d1
        and.w   %d3,%d1
        eor.w   %d1,(%a2)+
        lsr.w   #1,%d5                         | middle: odd word, then longs
        bcc.s   4f
        move.w  (%a3)+,(%a2)+
4:      add.w   %d5,%d5
        neg.w   %d5
        jmp     5f(%pc,%d5.w)
        .rept   NLONG
        move.l  (%a3)+,(%a2)+
        .endr
5:      move.w  (%a2),%d0                      | last word
        move.w  (%a3),%d1
        eor.w   %d0,%d1
        and.w   %d4,%d1
        eor.w   %d1,(%a2)
8:      lea     PF_RB(%a0),%a0
        lea     PF_RB(%a1),%a1
        dbra    %d6,1b
9:      movem.l (%sp)+,%d2-%d6/%a2-%a4
        rts
30:     addq.w  #1,%d5
        bmi.s   8b
        move.w  (%a2),%d0                      | one word
        move.w  (%a3),%d1
        eor.w   %d0,%d1
        and.w   %d3,%d1
        eor.w   %d1,(%a2)
        bra.s   8b

| void trap_rows(unsigned short *row, long n, long a, long astep, long b, long bstep,
|                const unsigned short *pat8, long y)
| For n rows from `row` (word 0 of playfield row y): fill pixels [a >> 16, b >> 16] (clamped)
| with pat8[y & 7], set the two edge pixels black when they are on the playfield, then step
| a and b (16.16) and y. Used for the ramps.
        .globl  trap_rows
trap_rows:
        movem.l %d2-%d7/%a2-%a4/%a6,-(%sp)     | 40 bytes saved
        move.l  44(%sp),%a0
        move.w  50(%sp),%d7
        move.l  52(%sp),%d0
        move.l  56(%sp),%a6                    | astep
        move.l  60(%sp),%d1
        move.l  68(%sp),%a1                    | patterns
        move.w  74(%sp),%a3                    | y
        lea     lmask(%pc),%a4                 | rmask = +32, bits = +64
        subq.w  #1,%d7
        bmi     9f
1:      move.l  %d0,%d4
        swap    %d4                            | x0
        move.l  %d1,%d5
        swap    %d5                            | x1
        tst.w   %d4
        bmi     20f
21:     cmp.w   #PF_W-1,%d5
        bgt     22f
23:     cmp.w   %d5,%d4
        bgt     8f
        move.w  %a3,%d6
        and.w   #7,%d6
        add.w   %d6,%d6
        move.w  0(%a1,%d6.w),%d6               | pattern word
        moveq   #15,%d2
        and.w   %d4,%d2
        add.w   %d2,%d2
        move.w  0(%a4,%d2.w),%d2               | left mask
        moveq   #15,%d3
        and.w   %d5,%d3
        add.w   %d3,%d3
        move.w  32(%a4,%d3.w),%d3              | right mask
        lsr.w   #4,%d4
        lsr.w   #4,%d5
        sub.w   %d4,%d5                        | words - 1
        add.w   %d4,%d4
        lea     0(%a0,%d4.w),%a2
        subq.w  #1,%d5                         | middle words (-1: one word)
        bmi.s   30f
        move.w  (%a2),%d4                      | first word: d ^= (d ^ p) & m
        eor.w   %d6,%d4
        and.w   %d2,%d4
        eor.w   %d4,(%a2)+
        subq.w  #1,%d5
        bmi.s   32f
33:     move.w  %d6,(%a2)+
        dbra    %d5,33b
32:     move.w  (%a2),%d4                      | last word
        eor.w   %d6,%d4
        and.w   %d3,%d4
        eor.w   %d4,(%a2)
        bra.s   40f
30:     and.w   %d3,%d2                        | one word
        move.w  (%a2),%d4
        eor.w   %d6,%d4
        and.w   %d2,%d4
        eor.w   %d4,(%a2)
40:     move.l  %d0,%d4                        | edge pixels (unclamped x, if on the playfield)
        swap    %d4
        cmp.w   #PF_W-1,%d4
        bhi.s   41f
        moveq   #15,%d2
        and.w   %d4,%d2
        add.w   %d2,%d2
        move.w  64(%a4,%d2.w),%d2
        lsr.w   #4,%d4
        add.w   %d4,%d4
        or.w    %d2,0(%a0,%d4.w)
41:     move.l  %d1,%d4
        swap    %d4
        cmp.w   #PF_W-1,%d4
        bhi.s   8f
        moveq   #15,%d2
        and.w   %d4,%d2
        add.w   %d2,%d2
        move.w  64(%a4,%d2.w),%d2
        lsr.w   #4,%d4
        add.w   %d4,%d4
        or.w    %d2,0(%a0,%d4.w)
8:      add.l   %a6,%d0
        add.l   64(%sp),%d1
        lea     PF_RB(%a0),%a0
        addq.w  #1,%a3
        dbra    %d7,1b
9:      movem.l (%sp)+,%d2-%d7/%a2-%a4/%a6
        rts
20:     moveq   #0,%d4
        bra     21b
22:     move.w  #PF_W-1,%d5
        bra     23b

| void rect_rows(unsigned short *row, short stride, short n, short x0, short x1,
|                const unsigned short *pat8, short y)
| Fill [x0, x1] (already clipped, x0 <= x1) of n rows starting at `row` (word 0 of the first
| row); row pattern pat8[y & 7], y increments per row; stride in bytes.
        .globl  rect_rows
rect_rows:
        movem.l %d2-%d7/%a2-%a3,-(%sp)         | 32 bytes saved
        move.l  36(%sp),%a0
        move.w  42(%sp),%a3                    | stride
        move.w  46(%sp),%d7                    | n
        move.w  50(%sp),%d4                    | x0
        move.w  54(%sp),%d5                    | x1
        move.l  56(%sp),%a1                    | patterns
        move.w  62(%sp),%d6                    | y
        lea     lmask(%pc),%a2
        moveq   #15,%d2
        and.w   %d4,%d2
        add.w   %d2,%d2
        move.w  0(%a2,%d2.w),%d2               | left mask
        moveq   #15,%d3
        and.w   %d5,%d3
        add.w   %d3,%d3
        move.w  32(%a2,%d3.w),%d3              | right mask
        lsr.w   #4,%d4
        lsr.w   #4,%d5
        sub.w   %d4,%d5                        | words - 1
        add.w   %d4,%d4
        adda.w  %d4,%a0
        subq.w  #1,%d7
        bmi     9f
        tst.w   %d5
        bne.s   3f
        and.w   %d3,%d2                        | single-word rectangles
1:      moveq   #7,%d0
        and.w   %d6,%d0
        add.w   %d0,%d0
        move.w  0(%a1,%d0.w),%d0
        move.w  (%a0),%d1
        eor.w   %d1,%d0
        and.w   %d2,%d0
        eor.w   %d0,(%a0)
        addq.w  #1,%d6
        adda.w  %a3,%a0
        dbra    %d7,1b
        bra.s   9f
3:      subq.w  #1,%d5                         | middle words (>= 0)
        moveq   #0,%d4
        lsr.w   #1,%d5                         | d5 = longs, X/C = odd word
        bcc.s   31f
        moveq   #2,%d4                         | 2 = an odd word to store first
31:     add.w   %d5,%d5
        neg.w   %d5                            | jump offset into the long block
4:      moveq   #7,%d0
        and.w   %d6,%d0
        add.w   %d0,%d0
        move.w  0(%a1,%d0.w),%d0               | pattern word
        move.l  %a0,%a2
        move.w  (%a2),%d1                      | first word
        eor.w   %d0,%d1
        and.w   %d2,%d1
        eor.w   %d1,(%a2)+
        move.w  %d0,%d1
        swap    %d1
        move.w  %d0,%d1                        | pattern long
        tst.w   %d4
        beq.s   5f
        move.w  %d0,(%a2)+
5:      jmp     6f(%pc,%d5.w)
        .rept   NLONG
        move.l  %d1,(%a2)+
        .endr
6:      move.w  (%a2),%d1                      | last word
        eor.w   %d0,%d1
        and.w   %d3,%d1
        eor.w   %d1,(%a2)
        addq.w  #1,%d6
        adda.w  %a3,%a0
        dbra    %d7,4b
9:      movem.l (%sp)+,%d2-%d7/%a2-%a3
        rts

| void spr_rows(unsigned short *dst, const unsigned short *src, long rows, long ww, long dskip)
| Masked sprite rows: per word dst = (dst & src[0]) | src[1]; ww words per row (1..32),
| then dst advances dskip more bytes.
        .globl  spr_rows
spr_rows:
        movem.l %d2/%a2-%a3,-(%sp)             | 12 bytes saved
        move.l  16(%sp),%a0
        move.l  20(%sp),%a1
        move.w  26(%sp),%d2                    | rows
        move.w  30(%sp),%d1                    | words per row
        move.w  34(%sp),%a2                    | dst skip (bytes)
        lsl.w   #3,%d1                         | 8 bytes of code per word
        lea     2f(%pc),%a3
        suba.w  %d1,%a3
        subq.w  #1,%d2
        bmi     9f
1:      jmp     (%a3)
        .rept   32
        move.w  (%a0),%d0
        and.w   (%a1)+,%d0
        or.w    (%a1)+,%d0
        move.w  %d0,(%a0)+
        .endr
2:      adda.w  %a2,%a0
        dbra    %d2,1b
9:      movem.l (%sp)+,%d2/%a2-%a3
        rts

| Vertical lines: void vl_xxx(unsigned short *p, long n, long mask) for n >= 1 rows, playfield
| stride. or = black, and = white (mask already inverted); l = long access, w = word access.
.macro  VLINE name, insn, size
        .globl  \name
\name:
        move.l  %d2,-(%sp)
        move.l  8(%sp),%a0
        move.w  14(%sp),%d1                    | n
        move.l  16(%sp),%d0                    | mask
        move.w  %d1,%d2
        neg.w   %d2
        and.w   #15,%d2                        | pairs to skip in the first pass
        add.w   %d2,%d2
        move.w  %d2,%a1
        add.w   %d2,%d2
        add.w   %a1,%d2                        | x 6 bytes of code
        add.w   #15,%d1
        lsr.w   #4,%d1
        subq.w  #1,%d1                         | passes - 1
        jmp     1f(%pc,%d2.w)
1:
        .rept   16
        \insn\().\size %d0,(%a0)
        lea     PF_RB(%a0),%a0
        .endr
        dbra    %d1,1b
        move.l  (%sp)+,%d2
        rts
.endm
        VLINE   vl_or_l, or, l
        VLINE   vl_and_l, and, l
        VLINE   vl_or_w, or, w
        VLINE   vl_and_w, and, w

        .even
lmask:  .short  0xFFFF,0x7FFF,0x3FFF,0x1FFF,0x0FFF,0x07FF,0x03FF,0x01FF
        .short  0x00FF,0x007F,0x003F,0x001F,0x000F,0x0007,0x0003,0x0001
rmask:  .short  0x8000,0xC000,0xE000,0xF000,0xF800,0xFC00,0xFE00,0xFF00
        .short  0xFF80,0xFFC0,0xFFE0,0xFFF0,0xFFF8,0xFFFC,0xFFFE,0xFFFF
bits:   .short  0x8000,0x4000,0x2000,0x1000,0x0800,0x0400,0x0200,0x0100
        .short  0x0080,0x0040,0x0020,0x0010,0x0008,0x0004,0x0002,0x0001

| sprite.s: the sprite drawer's inner part for the 68000 (obj.c walk() and spans(), the same
| results; verified with tests/sim68k.py sprites). NOT BUILT: measured no faster than the C
| (409K vs 413K cycles for five animals, 2026-10-17); obj.c uses it only with -DSPRITE_ASM. One column group: its runs from the top down to the sprite's foot, merged into the
| rows where the terrain is farther than the sprite, each filled with the picture rows' masks.
|
| void sprite_walk_asm(SpanCtx *c, const RRun *r0, long start, long dd, long ylo, long lim)
|   SpanCtx (obj.c): ys[33] at 0, bm[32] at 66, wm[32] at 98, dim at 130, cb at 134, ph at 138,
|   jj at 140. A run (12 bytes): yend 0, d 6, next 8 (byte offset from r0).

        .text
        .globl  sprite_walk_asm
        .equ    YS, 0
        .equ    BM, 66
        .equ    WM, 98
        .equ    DIM, 130
        .equ    CB, 134
        .equ    PH, 138
        .equ    JJ, 140

sprite_walk_asm:
        movem.l %d2-%d7/%a2-%a6,-(%sp)      | 44 bytes
        movea.l 48(%sp),%a4                 | c
        movea.l 52(%sp),%a5                 | r0
        movea.l %a5,%a3
        adda.l  56(%sp),%a3                 | r = r0 + start
        move.w  62(%sp),%d3                 | dd
        move.w  66(%sp),%d5                 | ylo
        move.w  70(%sp),%d6                 | lim
        moveq   #0,%d1                      | y: this run's first row
        moveq   #-1,%d2                     | vs: where the visible rows began (-1: none)
wloop:  move.w  (%a3),%d4                   | ye
        cmp.w   6(%a3),%d3
        bge.s   wnear                       | r->d <= dd: the terrain is in front
        tst.w   %d2
        bpl.s   wcont
        move.w  %d1,%d2                     | visible from y on
        bra.s   wcont
wnear:  tst.w   %d2
        bmi.s   wcont
        cmp.w   %d5,%d1
        ble.s   1f                          | (it ended above ylo)
        move.w  %d2,%d0                     | [max(vs, ylo), y)
        cmp.w   %d5,%d0
        bge.s   2f
        move.w  %d5,%d0
2:      move.w  %d1,%d7
        bsr     spans
1:      moveq   #-1,%d2
wcont:  cmp.w   %d6,%d4
        bge.s   wend                        | ye >= lim
        move.w  %d4,%d1
        movea.w 8(%a3),%a3
        adda.l  %a5,%a3
        bra.s   wloop
wend:   tst.w   %d2
        bmi.s   wout
        move.w  %d2,%d0                     | [max(vs, ylo), lim)
        cmp.w   %d5,%d0
        bge.s   3f
        move.w  %d5,%d0
3:      move.w  %d6,%d7
        bsr     spans
wout:   movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

| spans: the picture rows over screen rows [d0, d7) of the group (c in a4). Keeps d0-d7 except
| d1-d6 restored, a3, a4, a5.
spans:  movem.l %d1-%d6/%a3/%a5,-(%sp)
        move.w  PH(%a4),%d6                 | ph
        move.w  JJ(%a4),%d5                 | jj
        lea     YS+2(%a4),%a1
        adda.w  %d5,%a1
        adda.w  %d5,%a1                     | &ys[jj + 1]
s1:     cmp.w   %d6,%d5
        bge.s   s2
        cmp.w   (%a1),%d0
        blt.s   s2                          | ya < ys[jj + 1]
        addq.w  #1,%d5
        addq.l  #2,%a1
        bra.s   s1
s2:     move.w  %d5,JJ(%a4)
        subq.l  #2,%a1                      | &ys[j], j = jj
        lea     BM(%a4),%a2
        adda.w  %d5,%a2                     | &bm[j]
        movea.l CB(%a4),%a6
        movea.l DIM(%a4),%a5                | the dither (0: none)
sj:     cmp.w   %d6,%d5
        bge     sdone
        move.w  (%a1)+,%d1                  | ys[j]
        cmp.w   %d7,%d1
        bge     sdone                       | at or below yb
        move.w  (%a1),%d2                   | ys[j + 1]
        cmp.w   %d0,%d1
        bge.s   1f
        move.w  %d0,%d1                     | y0 = max(ys[j], ya)
1:      cmp.w   %d7,%d2
        ble.s   2f
        move.w  %d7,%d2                     | y1 = min(ys[j + 1], yb)
2:      move.b  WM-BM(%a2),%d3              | w: the drawn bits
        beq     snext
        sub.w   %d1,%d2                     | n rows
        ble     snext
        move.b  (%a2),%d4                   | k: the black bits
        movea.l %a6,%a0
        adda.w  %d1,%a0
        adda.w  %d1,%a0                     | p = cb + 2 * y0
        cmpa.w  #0,%a5
        bne.s   sdim
        cmp.b   #-1,%d3
        bne.s   smask
        move.w  %d2,%d1                     | whole bytes: k, 4 rows a turn
        lsr.w   #2,%d1
        beq.s   4f
        subq.w  #1,%d1
3:      move.b  %d4,(%a0)
        move.b  %d4,2(%a0)
        move.b  %d4,4(%a0)
        move.b  %d4,6(%a0)
        addq.l  #8,%a0
        dbra    %d1,3b
4:      and.w   #3,%d2
        beq     snext
        subq.w  #1,%d2
5:      move.b  %d4,(%a0)
        addq.l  #2,%a0
        dbra    %d2,5b
        bra     snext
smask:  not.b   %d3                         | the bits kept
        subq.w  #1,%d2
6:      move.b  (%a0),%d1
        and.b   %d3,%d1
        or.b    %d4,%d1
        move.b  %d1,(%a0)
        addq.l  #2,%a0
        dbra    %d2,6b
        bra     snext
sdim:   movem.w %d0/%d6,-(%sp)              | dark places: white bits dithered, row by row
        move.b  %d3,%d6
        not.b   %d6
        subq.w  #1,%d2
7:      move.w  %d1,%d0
        and.w   #7,%d0
        move.b  0(%a5,%d0.w),%d0
        and.b   %d3,%d0
        or.b    %d4,%d0
        and.b   %d6,(%a0)
        or.b    %d0,(%a0)
        addq.l  #2,%a0
        addq.w  #1,%d1
        dbra    %d2,7b
        movem.w (%sp)+,%d0/%d6
snext:  addq.w  #1,%d5
        addq.l  #1,%a2
        bra     sj
sdone:  movem.l (%sp)+,%d1-%d6/%a3/%a5
        rts

| segs.s: a column's segment list for the 68000 (the same as make_segs in world.c).
|
| short make_segs_asm(const unsigned char *b, Seg *s)
|   b: the column's WY blocks (y = 0 first); s: room for WY + 1 segments. Writes the segments top
|   first (top, bot, type, flags: TOPEXP 1 air above, BOTEXP 2 air below, bits 2-7 the count of
|   segments to the next run top or the end), then a terminator with top = 0. Returns how many.
|   The type byte's bits 5-6 hold the light class: 0 for the first segment, 1 under leaves or a
|   log, 2 under anything else (and the terminator: 0, 0, 2 << 5, 0).

        .text
        .globl  make_segs_asm
        .equ    WY, 48
        .equ    LOG, 5
        .equ    LEAVES, 7

make_segs_asm:
        movem.l %d2-%d4/%a2,-(%sp)
        movea.l 20(%sp),%a0             | b
        movea.l 24(%sp),%a1             | s
        movea.l %a1,%a2                 | the first segment
        lea     WY(%a0),%a0             | just above the top
        moveq   #WY-1,%d2               | y of the byte read next
skip:   tst.b   -(%a0)                  | air above the next run
        dbne    %d2,skip
        beq.s   done                    | (air to the bottom)
run:    move.b  (%a0),%d0               | a run of type d0 from y = d2 (a0 at it)
        move.b  %d2,%d3
        addq.b  #1,%d3
        move.b  %d3,(%a1)               | top = y + 1
        move.b  %d0,2(%a1)
        moveq   #0,%d4
        cmp.b   #WY,%d3
        beq.s   1f
        tst.b   1(%a0)
        bne.s   2f
1:      moveq   #1,%d4                  | TOPEXP
2:      subq.w  #1,%d2
        bmi.s   3f                      | down to y = 0
4:      cmp.b   -(%a0),%d0              | down while the same type
        dbne    %d2,4b
3:      move.b  %d2,%d3
        addq.b  #1,%d3
        move.b  %d3,1(%a1)              | bot = y + 1
        addq.l  #4,%a1
        tst.w   %d2
        bmi.s   7f                      | (to the bottom)
        tst.b   (%a0)
        bne.s   5f
        addq.b  #2,%d4                  | air below: BOTEXP
        move.b  %d4,-1(%a1)
        subq.w  #1,%d2
        bpl.s   skip
        bra.s   done
5:      move.b  %d4,-1(%a1)             | another type right below: its run starts here
        bra.s   run
7:      move.b  %d4,-1(%a1)
done:   move.l  #0x4000,(%a1)           | the terminator
        movea.l %a1,%a0                 | skip counts and light classes, from the end backwards
        moveq   #0,%d3
5:      cmpa.l  %a2,%a0
        bls.s   7f
        tst.b   (%a0)                   | the next segment: the end, or a run top?
        beq.s   6f
        btst    #0,3(%a0)
        beq.s   8f
6:      moveq   #0,%d3
8:      addq.b  #1,%d3
        move.b  %d3,%d4
        lsl.b   #2,%d4
        or.b    %d4,-1(%a0)
        subq.l  #4,%a0                  | a0: this segment
        cmpa.l  %a2,%a0
        bls.s   7f                      | the first: open to the sky
        moveq   #0x40,%d0               | covered,
        move.b  -2(%a0),%d1             | (the one above: its class not set yet)
        cmp.b   #LEAVES,%d1
        beq.s   9f
        cmp.b   #LOG,%d1
        bne.s   10f
9:      moveq   #0x20,%d0               | or under a tree
10:     or.b    %d0,2(%a0)
        bra.s   5b
7:      move.l  %a1,%d0
        sub.l   %a2,%d0
        lsr.l   #2,%d0
        movem.l (%sp)+,%d2-%d4/%a2
        rts

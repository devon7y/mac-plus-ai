| unpack.s: a save file's world for the 68000 (the same result as world_unpack_c in world.c).
|
| short unpack_asm(UnpackCtx *u)
|   The world part of a save (world.h): a dictionary of distinct columns. Per column, its id: one
|   seen before copies the first such column's 48 blocks (12 long moves) and shares its segment
|   list; a new one decodes its record (top, runs top-down) into its blocks and a new segment list
|   (top first; light class in the type byte, TOPEXP / BOTEXP and the skip counts in the flags,
|   as make_segs_c) and notes both in the table. Writes bw_cols (border excluded). 0, or -1 if
|   the data is bad. u->rec and u->heap come back pointing after what was used. The caller sets
|   the border, coarse map and capacities (0: shared) and lights torch-lit columns after.

        .text
        .globl  unpack_asm
        .equ    WX, 128
        .equ    WY, 48
        .equ    PWX, 176
        .equ    NBLOCKS, 29
        .equ    LOG, 5
        .equ    LEAVES, 7

| ---- UnpackCtx (world.c)
        .equ    U_INDEX, 0              | the columns' ids (words)
        .equ    U_REC, 4                | the next record
        .equ    U_RECEND, 8
        .equ    U_BLOCKS, 12
        .equ    U_HEAP, 16              | the next free segment
        .equ    U_TAB, 20               | per id: its first column's blocks, its list (8 bytes)
        .equ    U_COLS, 24              | &bw_cols[BORDER * PWX + BORDER]
        .equ    U_NDICT, 28             | (word) how many ids

| registers: a0 ids, a1 the column's blocks, a2 the next free segment, a4 u, a6 its bw_cols entry,
| d5 the table, d6 ids seen, d7 the column
unpack_asm:
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        movea.l 48(%sp),%a4
        movea.l U_INDEX(%a4),%a0
        movea.l U_BLOCKS(%a4),%a1
        movea.l U_HEAP(%a4),%a2
        move.l  U_TAB(%a4),%d5
        movea.l U_COLS(%a4),%a6
        moveq   #0,%d6
        moveq   #0,%d7
col:    move.w  (%a0)+,%d0              | its id
        cmp.w   %d6,%d0
        bhs     new
        movea.l %d5,%a3                 | seen before: the same blocks and list
        ext.l   %d0
        lsl.l   #3,%d0
        move.l  4(%a3,%d0.l),(%a6)+
        movea.l 0(%a3,%d0.l),%a3
        .rept   12
        move.l  (%a3)+,(%a1)+
        .endr
next:   addq.w  #1,%d7
        moveq   #WX-1,%d0
        and.w   %d7,%d0
        bne.s   1f
        lea     4*(PWX-WX)(%a6),%a6     | (the next row of bw_cols)
1:      cmp.w   #WX*WX,%d7
        blo.s   col
        move.l  %a2,U_HEAP(%a4)
        moveq   #0,%d0
        bra.s   99f
bad:    moveq   #-1,%d0
99:     movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

new:    bne.s   bad                     | (an id not seen yet must be the next one)
        cmp.w   U_NDICT(%a4),%d6
        bhs.s   bad
        movea.l %d5,%a3
        ext.l   %d0
        lsl.l   #3,%d0
        move.l  %a1,0(%a3,%d0.l)        | its blocks
        move.l  %a2,4(%a3,%d0.l)        | its list
        move.l  %a2,(%a6)+
        addq.w  #1,%d6
        movem.l %d5/%a0,-(%sp)          | (the table and the ids: d5 and a0 are needed below)
        movea.l U_REC(%a4),%a0
        move.l  %a2,-(%sp)              | the start of the group of segments (the first: the list)
        cmpa.l  U_RECEND(%a4),%a0
        bhs     badr
        moveq   #0,%d4
        move.b  (%a0)+,%d4              | y = its top
        cmp.w   #WY,%d4
        bhi     badr
        lea     WY(%a1),%a3             | air above it
        moveq   #WY,%d0
        sub.w   %d4,%d0
        moveq   #0,%d3
        add.w   %d0,%d0
        neg.w   %d0
        jmp     2f(%pc,%d0.w)
        .rept   48
        move.b  %d3,-(%a3)
        .endr
2:      moveq   #1,%d1                  | the first segment: TOPEXP,
        moveq   #0,%d5                  | class 0 (open to the sky)
        tst.w   %d4
        beq     rdone
run:    moveq   #0,%d3                  | the runs, top down to y 0 (the record is checked at
        move.b  (%a0)+,%d3              |  its end: the buffer has room past it)
        bmi.s   3f                      | (bit 7: it reaches y 0)
        moveq   #0,%d2
        move.b  %d4,%d2
        sub.b   (%a0)+,%d2              | k = y - its length
        beq     badr
        cmp.b   %d4,%d2
        bhs     badr
        bra.s   4f
3:      and.b   #0x7F,%d3
        moveq   #0,%d2
4:      cmp.b   #NBLOCKS,%d3
        bhs     badr
        lea     0(%a1,%d4.w),%a3        | blocks [k, y)
        move.w  %d4,%d0
        sub.w   %d2,%d0
        add.w   %d0,%d0
        neg.w   %d0
        jmp     5f(%pc,%d0.w)
        .rept   48
        move.b  %d3,-(%a3)
        .endr
5:      tst.b   %d3
        beq.s   air
        tst.b   %d1                     | after air: the group above is done (skip counts)
        beq.s   6f
        bsr.s   group
6:      move.b  %d4,(%a2)+              | a segment: top, bottom,
        move.b  %d2,(%a2)+
        or.b    %d3,%d5                 | type | its light class,
        move.b  %d5,(%a2)+
        move.b  %d1,(%a2)+              | TOPEXP after air (or the first)
        moveq   #0,%d1
        moveq   #0x40,%d5               | the next one's class: covered,
        cmp.b   #LEAVES,%d3
        beq.s   7f
        cmp.b   #LOG,%d3
        bne.s   8f
7:      moveq   #0x20,%d5               | or under a tree
        bra.s   8f
air:    cmpa.l  4(%sp),%a2              | (a column's first run is never air)
        beq     badr
        bset    #1,-1(%a2)              | BOTEXP: air below the segment above
        moveq   #1,%d1
8:      move.w  %d2,%d4                 | y = k
        bne     run
rdone:  move.l  #0x00004000,(%a2)       | the terminator: top 0, bot 0, covered, flags 0
        bsr.s   group                   | (the last group's skip counts)
        addq.l  #4,%sp
        addq.l  #4,%a2                  | (shared lists: no room to grow)
        cmpa.l  U_RECEND(%a4),%a0       | (the record stayed inside the data)
        bhi.s   badr
        move.l  %a0,U_REC(%a4)
        movem.l (%sp)+,%d5/%a0
        cmpa.l  %d5,%a2                 | (the heap reached the table: never seen)
        bhi     bad
        lea     WY(%a1),%a1
        bra     next

| the skip counts of the group of segments [4(sp), a2): each segment's count of segments to a2
| (the next run top or the terminator) in flag bits 2-7; the next group starts at a2
group:  movea.l 4(%sp),%a3
        move.l  %a2,%d0
        sub.l   %a3,%d0                 | 4 * the count
        beq.s   2f
1:      or.b    %d0,3(%a3)
        addq.l  #4,%a3
        subq.w  #4,%d0
        bne.s   1b
2:      move.l  %a2,4(%sp)
        rts

badr:   addq.l  #4,%sp
        movem.l (%sp)+,%d5/%a0
        bra     bad

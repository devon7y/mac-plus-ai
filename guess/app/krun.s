| Guess My Doodle: position loop for generated layer code (see engine.c).
| void g_krun(const void *code, void *a0, void *a1, long nx, long ny, long ixs, long irs, long oxs, long ors)
| Calls code once per grid position with a0/a1 = the position's input/output; the code uses d0-d7
| and restores a0/a1.
        .text
        .globl  g_krun
g_krun: movem.l %d2-%d7/%a2-%a6,-(%sp)
        lea     48(%sp),%a5             | args
        movea.l (%a5)+,%a6              | code
        movea.l (%a5)+,%a0
        movea.l (%a5)+,%a1
        move.l  4(%a5),-(%sp)           | rows left
        movea.l 8(%a5),%a2              | ixs
        movea.l 16(%a5),%a3             | oxs
1:      move.l  (%a5),-(%sp)            | columns left
2:      jsr     (%a6)
        adda.l  %a2,%a0
        adda.l  %a3,%a1
        subq.l  #1,(%sp)
        bne.s   2b
        addq.l  #4,%sp
        adda.l  12(%a5),%a0             | irs
        adda.l  20(%a5),%a1             | ors
        subq.l  #1,(%sp)
        bne.s   1b
        addq.l  #4,%sp
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

| void g_kpool(code, rq, a0, a1, nx, ny, ixs, irs, oxs, ors, sx, sy, nfill)
| Pooled conv: per output position, fill nfill longs at a1 with 0x80008000, call code at the four
| positions a0, a0+sx, a0+sx+sy, a0+sy of the pooling window (it keeps the max raw sums at a1),
| then rq (requantises them in place).
        .globl  g_kpool
g_kpool: movem.l %d2-%d7/%a2-%a6,-(%sp)
        lea     48(%sp),%a5
        movea.l (%a5)+,%a6              | code
        movea.l (%a5)+,%a4              | rq
        movea.l (%a5)+,%a0
        movea.l (%a5)+,%a1
        | a5 -> nx 0, ny 4, ixs 8, irs 12, oxs 16, ors 20, sx 24, sy 28, nfill 32
        move.l  4(%a5),-(%sp)           | rows left
1:      move.l  (%a5),-(%sp)            | columns left
2:      move.l  32(%a5),%d0
        movea.l %a1,%a2
        move.l  #0x80008000,%d1
3:      move.l  %d1,(%a2)+
        subq.l  #1,%d0
        bne.s   3b
        jsr     (%a6)
        adda.l  24(%a5),%a0
        jsr     (%a6)
        adda.l  28(%a5),%a0
        jsr     (%a6)
        suba.l  24(%a5),%a0
        jsr     (%a6)
        suba.l  28(%a5),%a0
        jsr     (%a4)
        adda.l  8(%a5),%a0
        adda.l  16(%a5),%a1
        subq.l  #1,(%sp)
        bne.s   2b
        addq.l  #4,%sp
        adda.l  12(%a5),%a0
        adda.l  20(%a5),%a1
        subq.l  #1,(%sp)
        bne.s   1b
        addq.l  #4,%sp
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

| void g_max4(short *o, const short *r0, const short *r1, const short *r2, const short *r3, long C)
| o[c] = max(r0[c], r1[c], r2[c], r3[c]), C > 0
        .globl  g_max4
g_max4: movem.l %d2/%a2-%a4,-(%sp)
        movem.l 20(%sp),%a0-%a4
        move.l  40(%sp),%d1
        subq.w  #1,%d1
1:      move.w  (%a1)+,%d0
        move.w  (%a2)+,%d2
        cmp.w   %d2,%d0
        bge.s   2f
        move.w  %d2,%d0
2:      move.w  (%a3)+,%d2
        cmp.w   %d2,%d0
        bge.s   3f
        move.w  %d2,%d0
3:      move.w  (%a4)+,%d2
        cmp.w   %d2,%d0
        bge.s   4f
        move.w  %d2,%d0
4:      move.w  %d0,(%a0)+
        dbra    %d1,1b
        movem.l (%sp)+,%d2/%a2-%a4
        rts

| void g_trow(short *o, const unsigned long *rows4, const short *const *trow, long C)
| The first layer and its 2x2 pool, one output row of 16 positions. rows4: the input rows 2Y-1,
| 2Y, 2Y+1, 2Y+2 (bit 31 = column 0; 0 outside the image). Window X covers columns 2X-1..2X+2:
| after shifting a row right by 1 (column -1 = 0 on top) and left by 2X, they are its top 4 bits.
| Pattern index = rows (r0, r1, r2) x columns (c0, c1, c2), bit 8 = (r0, c0). Each position gets
| the table's blank row when its 4x4 window is empty, else the max of the four table rows.
        .globl  g_trow
g_trow: movem.l %d2-%d7/%a2-%a6,-(%sp)
        movea.l 48(%sp),%a0             | o
        movea.l 52(%sp),%a1             | rows4
        movea.l 56(%sp),%a5             | trow
        move.l  60(%sp),%d7
        subq.w  #1,%d7                  | C - 1
        movem.l (%a1),%d2-%d5           | the four rows
        lsr.l   #1,%d2
        lsr.l   #1,%d3
        lsr.l   #1,%d4
        lsr.l   #1,%d5
        moveq   #15,%d6
1:      move.l  %d2,%d0
        or.l    %d3,%d0
        or.l    %d4,%d0
        or.l    %d5,%d0
        andi.l  #0xF0000000,%d0
        bne.s   3f
        movea.l (%a5),%a2               | blank
        move.w  %d7,%d1
2:      move.w  (%a2)+,(%a0)+
        dbra    %d1,2b
        bra     8f
3:      | i0: rows d2,d3,d4, columns 2X-1..2X+1 (rol #3); i1: columns 2X..2X+2 (rol #4)
        | i2, i3: rows d3,d4,d5
        move.l  %d2,%d0
        rol.l   #3,%d0
        andi.w  #7,%d0
        lsl.w   #3,%d0
        move.l  %d3,%d1
        rol.l   #3,%d1
        andi.w  #7,%d1
        or.w    %d1,%d0
        lsl.w   #3,%d0
        move.l  %d4,%d1
        rol.l   #3,%d1
        andi.w  #7,%d1
        or.w    %d1,%d0
        add.w   %d0,%d0
        add.w   %d0,%d0
        movea.l 0(%a5,%d0.w),%a2
        move.l  %d2,%d0
        rol.l   #4,%d0
        andi.w  #7,%d0
        lsl.w   #3,%d0
        move.l  %d3,%d1
        rol.l   #4,%d1
        andi.w  #7,%d1
        or.w    %d1,%d0
        lsl.w   #3,%d0
        move.l  %d4,%d1
        rol.l   #4,%d1
        andi.w  #7,%d1
        or.w    %d1,%d0
        add.w   %d0,%d0
        add.w   %d0,%d0
        movea.l 0(%a5,%d0.w),%a3
        move.l  %d3,%d0
        rol.l   #3,%d0
        andi.w  #7,%d0
        lsl.w   #3,%d0
        move.l  %d4,%d1
        rol.l   #3,%d1
        andi.w  #7,%d1
        or.w    %d1,%d0
        lsl.w   #3,%d0
        move.l  %d5,%d1
        rol.l   #3,%d1
        andi.w  #7,%d1
        or.w    %d1,%d0
        add.w   %d0,%d0
        add.w   %d0,%d0
        movea.l 0(%a5,%d0.w),%a4
        move.l  %d3,%d0
        rol.l   #4,%d0
        andi.w  #7,%d0
        lsl.w   #3,%d0
        move.l  %d4,%d1
        rol.l   #4,%d1
        andi.w  #7,%d1
        or.w    %d1,%d0
        lsl.w   #3,%d0
        move.l  %d5,%d1
        rol.l   #4,%d1
        andi.w  #7,%d1
        or.w    %d1,%d0
        add.w   %d0,%d0
        add.w   %d0,%d0
        movea.l 0(%a5,%d0.w),%a6
        move.w  %d7,%d1
        swap    %d6                     | (the low word of d6 is the X counter; its high word is free)
4:      move.w  (%a2)+,%d0
        move.w  (%a3)+,%d6
        cmp.w   %d6,%d0
        bge.s   5f
        move.w  %d6,%d0
5:      move.w  (%a4)+,%d6
        cmp.w   %d6,%d0
        bge.s   6f
        move.w  %d6,%d0
6:      move.w  (%a6)+,%d6
        cmp.w   %d6,%d0
        bge.s   7f
        move.w  %d6,%d0
7:      move.w  %d0,(%a0)+
        dbra    %d1,4b
        swap    %d6
8:      lsl.l   #2,%d2
        lsl.l   #2,%d3
        lsl.l   #2,%d4
        lsl.l   #2,%d5
        dbra    %d6,1b
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

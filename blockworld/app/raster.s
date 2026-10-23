| raster.s: Blockworld edge pass and rasteriser for the 68000 (same output as r_edges and
| r_raster in render.c).
|
| void edges_asm(EdgeCtx *e)
|   per column: walk its run list and the left neighbour's together; uncovered runs get the
|   sky / haze / fog background by row band; R_EDGE where the two surface keys differ; equal
|   neighbouring ids merge. Output: the compact (end row, id) lists r_comp.
| void raster_asm(RastCtx *r)
|   per byte (two 4-pixel columns): merge the two compact lists; each merged segment's 8 row
|   values (one per row & 7) are PL[id a][p] | PR[id b][p] (PL / PR: the pattern's high / low
|   nibble plus the column's edge pixel). Segments of up to 4 rows are written row by row;
|   longer ones load the 8 values into d0-d7 and jsr into a generated unrolled fill (one
|   "move.b dN,row*rb(a0)" per row; phases 0..7 in d0 d1 d2 d4 d5 d6 d7 d3) whose entry at the
|   end row is patched to rts for the call.

        .text
        .globl  edges_asm
        .globl  raster_asm

| ---- EdgeCtx
        .equ    E_RUNS, 0               | RRun r_runs[NC][R_MAXN]
        .equ    E_START, 4              | short r_start[NC]
        .equ    E_COMP, 8               | short r_comp[NC][R_MAXC * 2]
        .equ    E_NC, 12
        .equ    E_H, 14
        .equ    E_HZ, 16                | H0 - 24: haze from here
        .equ    E_H0, 18                | fog background from here
        .equ    RUNBYTES, 1440
        .equ    COMPBYTES, 480          | R_MAXC * 4
        .equ    EDGE, 1024
        .equ    SKY, 0
        .equ    HAZE, 1
        .equ    FOGBG, 2

| registers: a0 run (this column), a1 run (left column, or 0), a2 / a3 their columns' starts,
| a4 ctx, a5 output, d0 y, d1 this run's end, d2 the left run's end, d3 e, d4 id, d5 H,
| d6 entries written, d7 column
edges_asm:
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        movea.l 48(%sp),%a4
        moveq   #0,%d7
        move.w  E_H(%a4),%d5
ecol:   movea.l E_RUNS(%a4),%a2
        move.w  %d7,%d0
        mulu.w  #RUNBYTES,%d0
        adda.l  %d0,%a2                 | this column's runs
        movea.l E_START(%a4),%a6
        move.w  %d7,%d0
        add.w   %d0,%d0
        move.w  0(%a6,%d0.w),%d1
        lea     0(%a2,%d1.w),%a0        | its first run
        suba.l  %a1,%a1
        tst.w   %d7
        beq.s   1f
        lea     -RUNBYTES(%a2),%a3      | the left column
        move.w  -2(%a6,%d0.w),%d1
        lea     0(%a3,%d1.w),%a1
1:      movea.l E_COMP(%a4),%a5
        move.w  %d7,%d0
        mulu.w  #COMPBYTES,%d0
        adda.l  %d0,%a5
        moveq   #0,%d0                  | y
        moveq   #0,%d6                  | entries
eloop:  move.w  (%a0),%d1               | this run's end
        move.w  %d5,%d2                 | the left run's end (H when none)
        cmpa.w  #0,%a1
        beq.s   1f
        move.w  (%a1),%d2
1:      move.w  %d1,%d3
        cmp.w   %d2,%d3
        ble.s   2f
        move.w  %d2,%d3                 | e = min
2:      move.w  2(%a0),%d4              | id
        bpl.s   4f
        | uncovered: the background by row band
        cmp.w   E_HZ(%a4),%d0
        bge.s   3f
        moveq   #SKY,%d4
        cmp.w   E_HZ(%a4),%d3
        ble.s   4f
        move.w  E_HZ(%a4),%d3
        bra.s   4f
3:      cmp.w   E_H0(%a4),%d0
        bge.s   5f
        moveq   #HAZE,%d4
        cmp.w   E_H0(%a4),%d3
        ble.s   4f
        move.w  E_H0(%a4),%d3
        bra.s   4f
5:      moveq   #FOGBG,%d4
4:      cmpa.w  #0,%a1                  | an edge where the keys differ
        beq.s   6f
        move.w  4(%a0),%d2
        cmp.w   4(%a1),%d2
        beq.s   6f
        or.w    #EDGE,%d4
6:      tst.w   %d6                     | same id as the last entry: extend it
        beq.s   7f
        cmp.w   -2(%a5),%d4
        bne.s   7f
        move.w  %d3,-4(%a5)
        bra.s   8f
7:      cmp.w   #120,%d6                | R_MAXC
        bcc.s   8f
        move.w  %d3,(%a5)+
        move.w  %d4,(%a5)+
        addq.w  #1,%d6
8:      move.w  %d3,%d0                 | y = e
        cmp.w   (%a0),%d3
        bne.s   9f
        move.w  8(%a0),%d1              | next run (offset from the column's start)
        lea     0(%a2,%d1.w),%a0
9:      cmpa.w  #0,%a1
        beq.s   10f
        cmp.w   (%a1),%d3
        bne.s   10f
        move.w  8(%a1),%d1
        lea     0(%a3,%d1.w),%a1
10:     cmp.w   %d5,%d0
        blt     eloop
        addq.w  #1,%d7
        cmp.w   E_NC(%a4),%d7
        blt     ecol
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

| ---- RastCtx
        .equ    R_COMP, 0               | short r_comp[NC][R_MAXC * 2]
        .equ    R_BUF, 4                | the 1-bit buffer
        .equ    R_PL, 8                 | unsigned char PL[2048][8]
        .equ    R_PR, 12                | unsigned char PR[2048][8]
        .equ    R_CODE, 16              | the unrolled fill: (H + 1) entries of 4 bytes
        .equ    R_NB, 20                | bytes per row used (NC / 2)
        .equ    R_RB, 22                | buffer row bytes
        .equ    R_H, 24
        .equ    R_K, 26                 | the byte column
        .equ    R_SAVE, 28              | y, e, y * rb during a long fill
        .equ    R_SAVEE, 30
        .equ    R_SAVEP, 32

| registers: a0 this byte's column in the buffer, a1 / a2 the two compact lists, a3 PL,
| a4 ctx, a5 PR, a6 the fill code; d6 y, d7 y * rb, d3 e
raster_asm:
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        movea.l 48(%sp),%a4
        movea.l R_PL(%a4),%a3
        movea.l R_PR(%a4),%a5
        movea.l R_CODE(%a4),%a6
        clr.w   R_K(%a4)
rcol:   move.w  R_K(%a4),%d0
        movea.l R_COMP(%a4),%a1
        mulu.w  #2 * COMPBYTES,%d0
        adda.l  %d0,%a1                 | column 2k
        lea     COMPBYTES(%a1),%a2      | column 2k + 1
        move.w  R_K(%a4),%d0            | the byte column in the column-pair layout:
        lsr.w   #1,%d0                  | (k >> 1) * 2H + (k & 1)
        mulu.w  R_H(%a4),%d0
        add.l   %d0,%d0
        movea.l R_BUF(%a4),%a0
        adda.l  %d0,%a0
        btst    #0,R_K+1(%a4)
        beq.s   1f
        addq.l  #1,%a0
1:      moveq   #0,%d6                  | y
        moveq   #0,%d7                  | y * rb
rseg:   move.w  (%a1),%d3               | e = the nearer end
        cmp.w   (%a2),%d3
        ble.s   1f
        move.w  (%a2),%d3
1:      move.w  2(%a1),%d0
        lsl.w   #3,%d0                  | id a * 8
        move.w  2(%a2),%d1
        lsl.w   #3,%d1                  | id b * 8
        move.w  %d3,%d5
        sub.w   %d6,%d5                 | rows
        cmp.w   #4,%d5
        bgt.s   rlong
2:      move.w  %d6,%d2                 | short: row by row
        and.w   #7,%d2
        move.w  %d2,%d4
        add.w   %d0,%d2
        move.b  0(%a3,%d2.w),%d5
        add.w   %d1,%d4
        or.b    0(%a5,%d4.w),%d5
        move.b  %d5,0(%a0,%d7.w)
        add.w   R_RB(%a4),%d7
        addq.w  #1,%d6
        cmp.w   %d3,%d6
        blt.s   2b
        bra.s   rnext
rlong:  move.w  %d6,R_SAVE(%a4)         | long: values in d0-d7, then the unrolled fill
        move.w  %d3,R_SAVEE(%a4)
        movem.l %a1-%a2,-(%sp)
        lea     0(%a3,%d0.w),%a1
        lea     0(%a5,%d1.w),%a2
        move.b  (%a1)+,%d0
        or.b    (%a2)+,%d0
        move.b  (%a1)+,%d1
        or.b    (%a2)+,%d1
        move.b  (%a1)+,%d2
        or.b    (%a2)+,%d2
        move.b  (%a1)+,%d4
        or.b    (%a2)+,%d4
        move.b  (%a1)+,%d5
        or.b    (%a2)+,%d5
        move.b  (%a1)+,%d6
        or.b    (%a2)+,%d6
        move.b  (%a1)+,%d7
        or.b    (%a2)+,%d7
        move.b  (%a1)+,%d3
        or.b    (%a2)+,%d3
        movea.w R_SAVEE(%a4),%a1
        adda.w  %a1,%a1
        adda.w  %a1,%a1                 | e * 4
        movea.w R_SAVE(%a4),%a2
        adda.w  %a2,%a2
        adda.w  %a2,%a2                 | y * 4
        move.w  0(%a6,%a1.w),-(%sp)     | patch rts at row e
        move.w  #0x4E75,0(%a6,%a1.w)
        jsr     0(%a6,%a2.w)
        move.w  (%sp)+,0(%a6,%a1.w)
        movem.l (%sp)+,%a1-%a2
        move.w  R_SAVEE(%a4),%d3
        move.w  %d3,%d6                 | y = e
        move.w  %d3,%d7
        mulu.w  R_RB(%a4),%d7           | y * rb
rnext:  cmp.w   (%a1),%d3
        bne.s   3f
        addq.l  #4,%a1
3:      cmp.w   (%a2),%d3
        bne.s   4f
        addq.l  #4,%a2
4:      cmp.w   R_H(%a4),%d6
        blt     rseg
        addq.w  #1,R_K(%a4)
        move.w  R_K(%a4),%d0
        cmp.w   R_NB(%a4),%d0
        blt     rcol
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

| ======================================================================= moving frames
| void raster8_asm(Rast8Ctx *q): column 2j drawn 8 px wide as byte j, edges against column
| 2j - 2 (r_raster8 in render.c); the buffer in column pairs (row y of byte j at
| (j >> 1) * 2H + 2y + (j & 1)). The buffer is cleared first (the sky is white: its segments are
| skipped). Per column the two run lists are walked together (ends, ids, keys in registers);
| segments with the same idx (id | EDGE) merge; each is filled 4 rows at a time by MOVEP.L
| (an unrolled block entered Duff-style) from its pattern (p16: 8 bytes twice, inside the
| ctx), odd first / last rows by bytes.
        .globl  raster8_asm
        .globl  present_asm
        .equ    Q_RUNS, 0
        .equ    Q_START, 4
        .equ    Q_BUF, 8
        .equ    Q_NC, 16
        .equ    Q_H, 18
        .equ    Q_HZ, 20
        .equ    Q_H0, 22
        .equ    Q_J, 24
        .equ    Q_Y, 26                 | the current row
        .equ    Q_PS, 28                | the pending segment's first row
        .equ    Q_NI, 30
        .equ    Q_COL, 32
        .equ    Q_P16, 48
        .equ    Q_P2, 32816
        .equ    MPN, 72                 | MOVEP.L entries: 288 rows

| ADVA / ADVB: the next run of this column / the left one. A gap run gets its band's background
| and ends at the band's end; it is re-entered for the next band.
        .macro  ADVA
        cmp.w   (%a1),%d3
        bne.s   1f                      | (inside a gap run cut at a band edge)
        movea.w 8(%a1),%a1
        adda.l  %a3,%a1
        move.w  4(%a1),%d7
1:      move.w  (%a1),%d3
        move.w  2(%a1),%d5
        bpl.s   2f
        bsr     band8
2:
        .endm
        .macro  ADVB
        movea.w 8(%a2),%a2
        adda.l  %a5,%a2
        move.w  (%a2),%d4
        move.w  4(%a2),%d1
        .endm

| registers: a0 byte column, a1 / a2 runs of this column / the left one, a3 / a5 their columns'
| first runs, a4 ctx; d1 kb, d2 pending idx (-1: none), d3 ea, d4 eb, d5 ida, d7 ka; d0 / d6 /
| a6 scratch. Segments are filled top-down, a long one 4 rows at a time and possibly up to 3
| rows too far: the next segment (or column) writes those rows later. (The buffer needs 8
| bytes of slack.)
raster8_asm:
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        movea.l 48(%sp),%a4
        clr.w   Q_J(%a4)
r8col:  move.w  Q_J(%a4),%d0
        add.w   %d0,%d0                 | c = 2j
        movea.l Q_RUNS(%a4),%a3
        move.w  %d0,%d1
        mulu.w  #RUNBYTES,%d1
        adda.l  %d1,%a3                 | column c's runs
        movea.l Q_START(%a4),%a5
        add.w   %d0,%d0
        move.w  0(%a5,%d0.w),%d1
        lea     0(%a3,%d1.w),%a1        | its first run
        tst.w   Q_J(%a4)
        bne.s   1f
        movea.l %a3,%a5                 | column 0: compared with itself (no edges)
        movea.l %a1,%a2
        bra.s   2f
1:      move.w  -4(%a5,%d0.w),%d1       | column c - 2
        lea     -2*RUNBYTES(%a3),%a5
        lea     0(%a5,%d1.w),%a2
2:      move.w  Q_J(%a4),%d0            | this byte column: (j >> 1) * 2H + (j & 1)
        lsr.w   #1,%d0
        mulu.w  Q_H(%a4),%d0
        add.l   %d0,%d0
        movea.l Q_BUF(%a4),%a0
        adda.l  %d0,%a0
        btst    #0,Q_J+1(%a4)
        beq.s   3f
        addq.l  #1,%a0
3:      move.l  %a0,Q_COL(%a4)
        clr.w   Q_Y(%a4)
        clr.w   Q_PS(%a4)
        move.w  (%a1),%d3
        move.w  4(%a1),%d7
        move.w  2(%a1),%d5
        bpl.s   4f
        bsr     band8
4:      move.w  (%a2),%d4
        move.w  4(%a2),%d1
        move.w  %d5,%d2                 | the first segment is pending
        cmp.w   %d7,%d1
        beq.s   r8adv
        or.w    #EDGE,%d2
        bra.s   r8adv
r8step: move.w  %d5,%d0                 | this segment's idx
        cmp.w   %d7,%d1
        beq.s   1f
        or.w    #EDGE,%d0
1:      cmp.w   %d0,%d2
        beq.s   r8adv                   | the same as the pending one: it grows
        bsr     flush8
r8adv:  cmp.w   %d4,%d3
        blt.s   r8a
        bgt.s   r8b
        move.w  %d3,Q_Y(%a4)            | both runs end here
        cmp.w   Q_H(%a4),%d3
        bge.s   r8end
        ADVA
        ADVB
        bra.s   r8step
r8a:    move.w  %d3,Q_Y(%a4)
        ADVA
        bra.s   r8step
r8b:    move.w  %d4,Q_Y(%a4)
        ADVB
        bra.s   r8step
r8end:  bsr.s   flush8
        addq.w  #1,Q_J(%a4)
        move.w  Q_J(%a4),%d0
        add.w   %d0,%d0
        cmp.w   Q_NC(%a4),%d0
        blt     r8col
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

| band8: d5 = the background of the gap run a1 for the band holding row Q_Y; d3 = its end, cut
| at the band's end.
band8:  move.w  Q_Y(%a4),%d5
        cmp.w   Q_HZ(%a4),%d5
        bge.s   1f
        cmp.w   Q_HZ(%a4),%d3
        ble.s   2f
        move.w  Q_HZ(%a4),%d3
2:      moveq   #SKY,%d5
        rts
1:      cmp.w   Q_H0(%a4),%d5
        bge.s   3f
        cmp.w   Q_H0(%a4),%d3
        ble.s   4f
        move.w  Q_H0(%a4),%d3
4:      moveq   #HAZE,%d5
        rts
3:      moveq   #FOGBG,%d5
        rts

| flush8: rows [Q_PS, Q_Y) get idx d2; then the pending segment is (Q_Y, d0): d2 = d0.
| Keeps d0, d1, d3-d5, d7, a0. Clobbers d6, a6.
flush8: move.w  Q_Y(%a4),%d6
        sub.w   Q_PS(%a4),%d6
        subq.w  #1,%d6                  | rows - 1
        bne.s   f8l
        move.w  Q_PS(%a4),%d6           | one row: p16[idx][y & 7] at 2y
        and.w   #7,%d6
        lsl.w   #4,%d2
        add.w   %d6,%d2
        move.w  Q_PS(%a4),%d6
        add.w   %d6,%d6
        move.b  Q_P16(%a4,%d2.w),0(%a0,%d6.w)
        move.w  %d0,%d2
        move.w  Q_Y(%a4),Q_PS(%a4)
        rts
f8l:    move.w  %d0,Q_NI(%a4)
        move.w  %d6,%d0                 | rows - 1
        move.w  Q_PS(%a4),%d6           | the pattern from row PS, every other byte:
        and.w   #7,%d6                  | p2[id] + 2 (PS & 7)
        add.w   %d6,%d6
        lea     0(%a4,%d6.w),%a6
        moveq   #0,%d6
        move.w  %d2,%d6
        lsl.l   #5,%d6
        adda.l  %d6,%a6
        adda.l  #Q_P2,%a6               | p2[idx] + 2 (PS & 7)
        movep.l 0(%a6),%d6              | rows PS .. PS + 3
        movep.l 8(%a6),%d2              | rows PS + 4 .. PS + 7
        addq.w  #4,%d0
        and.w   #-4,%d0                 | 4m, m = ceil(rows / 4)
        lea     mpend(%pc),%a6
        suba.w  %d0,%a6                 | the block's entry MPN - m
        adda.w  %d0,%a0
        adda.w  %d0,%a0
        btst    #2,%d0
        beq.s   2f
        exg     %d6,%d2                 | an odd entry: its register holds the second long
2:      move.w  Q_PS(%a4),%d0
        add.w   %d0,%d0
        adda.w  %d0,%a0
        suba.w  #8*MPN,%a0              | so that the entry writes row PS
        jsr     (%a6)
        movea.l Q_COL(%a4),%a0
        move.w  Q_NI(%a4),%d0
        move.w  %d0,%d2
        move.w  Q_Y(%a4),Q_PS(%a4)
        rts

| the MOVEP block: entry e writes rows 4e .. 4e + 3 (relative) from d6 (e even) or d2 (e odd)
mpblk:
        .set    mpi, 0
        .rept   MPN/2
        movep.l %d6,mpi*16(%a0)
        movep.l %d2,mpi*16+8(%a0)
        .set    mpi, mpi+1
        .endr
mpend:  rts

| ======================================================================= Full size, moving
| void raster16_asm(Rast8Ctx *q): column 4j drawn 16 px wide as the word column j (row y at
| j * 2H + 2y), edges against column 4j - 4 (r_raster16 in render.c). The same walk as
| raster8_asm; each segment is filled 2 rows a long from p2, whose words are (pattern byte |
| edge pixel, pattern byte), possibly one row too far (written again by the next segment).
        .globl  raster16_asm
        .equ    Q_SV, 36
raster16_asm:
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        movea.l 48(%sp),%a4
        clr.w   Q_J(%a4)
r16col: move.w  Q_J(%a4),%d0
        add.w   %d0,%d0
        add.w   %d0,%d0                 | c = 4j
        movea.l Q_RUNS(%a4),%a3
        move.w  %d0,%d1
        mulu.w  #RUNBYTES,%d1
        adda.l  %d1,%a3                 | column c's runs
        movea.l Q_START(%a4),%a5
        add.w   %d0,%d0
        move.w  0(%a5,%d0.w),%d1
        lea     0(%a3,%d1.w),%a1        | its first run
        tst.w   Q_J(%a4)
        bne.s   1f
        movea.l %a3,%a5                 | column 0: compared with itself (no edges)
        movea.l %a1,%a2
        bra.s   2f
1:      move.w  -8(%a5,%d0.w),%d1       | column c - 4
        lea     -4*RUNBYTES(%a3),%a5
        lea     0(%a5,%d1.w),%a2
2:      move.w  Q_J(%a4),%d0            | this word column: j * 2H
        mulu.w  Q_H(%a4),%d0
        add.l   %d0,%d0
        movea.l Q_BUF(%a4),%a0
        adda.l  %d0,%a0
        move.l  %a0,Q_COL(%a4)
        clr.w   Q_Y(%a4)
        clr.w   Q_PS(%a4)
        move.w  (%a1),%d3
        move.w  4(%a1),%d7
        move.w  2(%a1),%d5
        bpl.s   4f
        bsr     band8
4:      move.w  (%a2),%d4
        move.w  4(%a2),%d1
        move.w  %d5,%d2                 | the first segment is pending
        cmp.w   %d7,%d1
        beq.s   r16adv
        or.w    #EDGE,%d2
        bra.s   r16adv
r16stp: move.w  %d5,%d0                 | this segment's idx
        cmp.w   %d7,%d1
        beq.s   1f
        or.w    #EDGE,%d0
1:      cmp.w   %d0,%d2
        beq.s   r16adv                  | the same as the pending one: it grows
        bsr     flush16
r16adv: cmp.w   %d4,%d3
        blt.s   r16a
        bgt.s   r16b
        move.w  %d3,Q_Y(%a4)            | both runs end here
        cmp.w   Q_H(%a4),%d3
        bge.s   r16end
        ADVA
        ADVB
        bra.s   r16stp
r16a:   move.w  %d3,Q_Y(%a4)
        ADVA
        bra.s   r16stp
r16b:   move.w  %d4,Q_Y(%a4)
        ADVB
        bra.s   r16stp
r16end: bsr     flush16
        addq.w  #1,Q_J(%a4)
        move.w  Q_J(%a4),%d0
        lsl.w   #2,%d0
        cmp.w   Q_NC(%a4),%d0
        blt     r16col
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

| flush16: rows [Q_PS, Q_Y) get idx d2, m = ceil(rows / 2) longs entered Duff-style into the
| block of 144 (registers d0, d1, d2, d6 in turn, loaded from p2[idx] at the row the entry's
| register writes); then the pending segment is (Q_Y, d0): d2 = d0.
| Keeps d0, d1, d3-d5, d7, a0. Clobbers d6, a6.
flush16:
        move.w  %d0,Q_NI(%a4)
        move.w  %d1,Q_SV(%a4)
        move.w  Q_Y(%a4),%d6
        sub.w   Q_PS(%a4),%d6
        addq.w  #1,%d6
        lsr.w   #1,%d6
        add.w   %d6,%d6                 | 2m
        movea.w #288,%a6
        suba.w  %d6,%a6                 | the entry: 2 (144 - m) bytes into the block
        add.w   Q_PS(%a4),%d6
        and.w   #7,%d6                  | s = (PS + 2m) & 7: the row register d0 holds
        add.w   %d6,%d6
        moveq   #0,%d1
        move.w  %d2,%d1
        lsl.l   #5,%d1                  | idx * 32
        movea.l %a4,%a0
        adda.l  #Q_P2,%a0
        adda.l  %d1,%a0
        adda.w  %d6,%a0                 | p2[idx] + 2s
        movem.l (%a0),%d0-%d2/%d6       | rows s .. s + 7
        movea.l Q_COL(%a4),%a0
        adda.w  Q_PS(%a4),%a0
        adda.w  Q_PS(%a4),%a0           | row PS
        jmp     mlblk(%pc,%a6.w)
mlblk:
        .rept   36
        move.l  %d0,(%a0)+
        move.l  %d1,(%a0)+
        move.l  %d2,(%a0)+
        move.l  %d6,(%a0)+
        .endr
        move.w  Q_SV(%a4),%d1
        move.w  Q_NI(%a4),%d0
        move.w  %d0,%d2
        movea.l Q_COL(%a4),%a0
        move.w  Q_Y(%a4),Q_PS(%a4)
        rts

| void present_asm(code, src, dst, h - 1, skip): run the generated present loop (render.c
| r_pres: per row a word from each column pair, then src += 2, dst += skip)
present_asm:
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        movea.l 48(%sp),%a2
        movea.l 52(%sp),%a1
        movea.l 56(%sp),%a0
        move.w  62(%sp),%d0             | (short arguments: the low word of each slot)
        move.w  66(%sp),%d1
        jsr     (%a2)
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

| cast.s: Blockworld column caster for the 68000, same output as r_cast() in render.c.
|
| void cast_columns(RCtx *ctx)   (ctx filled by r_cast_asm: tables, view, eye cell)
|
| Per column: ray setup (DDA steps, distances), then
|   A. a tight DDA loop writing one 8-byte record per cell: entry key, Seg*, far distance;
|   B. the records processed: projections, early outs, painting pieces.
| Projection: Y = H0 - hi16(T[h] * recip[d]); T is the 8.8 height table for d >= 513 and the
| x16 saturated one for nearer distances (exact on screen; see render.c frame_setup).
|
| Columns start in fast mode: one uncovered gap [GSTART, GEND), pieces that reach its bottom
| are pushed on a bottom stack (from the column's end down), pieces that reach its top on a top
| stack (from its start up). The first piece strictly inside the gap switches the column to the
| general gap list (as r_paint in render.c). At the end the stacks are linked into one run list.
|
| Order within a cell (as engine/render.py): the segment across the eye level, the segments
| above the eye bottom-up, then the rest top-down. Pieces of one run (contiguous segments) never
| overlap, so each run is done in two passes: its visible sides are collected, then painted
| from the far end of the gap inwards (bottom-up below the eye, top-down above it), then the
| top / bottom face (rest first, then its edge line). Work skipped here but not in render.c only
| ever covers rows that are already covered.
|
| Registers in B: a0 record, a1 far height table, a2 segment, a3 near table, a4 ctx,
| a5 this column's runs, a6 scratch; d4 side chain (y or NONE), d5 H0, d6 rn, d7 rf.
| paint: d0 = y0, d1 = y1, PID/PKEY/DEPTH in ctx; clobbers d0-d3 and a6.

        .text
        .globl  cast_columns

        .equ    H, 0
        .equ    H0, 2
        .equ    NG, 4
        .equ    NN, 6
        .equ    GSTART, 8
        .equ    GEND, 10
        .equ    DEPTH, 12
        .equ    DN, 14
        .equ    DF, 16
        .equ    VF, 18
        .equ    TY1, 20
        .equ    FOGD, 22
        .equ    DMAX, 24
        .equ    NCACHE, 26
        .equ    PCH0, 28
        .equ    PCH1, 30
        .equ    PCY0, 32
        .equ    PCY1, 34
        .equ    NCH, 36
        .equ    CH0, 38
        .equ    CY0, 42
        .equ    SKEY, 46
        .equ    SFACE, 48
        .equ    PID, 50
        .equ    PKEY, 52
        .equ    SSH, 54                 | the side's type id and light (TID entry)
        .equ    DIRX, 56
        .equ    DIRZ, 58
        .equ    PLX, 60
        .equ    PLZ, 62
        .equ    FX, 64
        .equ    FZ, 66
        .equ    KX0, 68
        .equ    KZ0, 70
        .equ    NCOLS, 72
        .equ    COL, 74
        .equ    VAR0, 76
        .equ    PWX4, 80
        .equ    RUNS, 84
        .equ    T8, 88
        .equ    T16, 92
        .equ    B8, 96
        .equ    B16, 100
        .equ    RECIP, 104
        .equ    CAMX, 108
        .equ    CP0, 112
        .equ    RECEND, 120
        .equ    SEG0, 124
        .equ    SEGK, 128
        .equ    LASTG, 132
        .equ    RKEY, 136
        .equ    MODE, 138
        .equ    SAVESP, 140
        .equ    BOTLIM, 144
        .equ    PBUF, 148
        .equ    TOPSP, 152
        .equ    BOTSP, 156
        .equ    SLOW, 160
        .equ    NNLIM, 162
        .equ    RSTART, 164
        .equ    SBP, 168
        .equ    RUNSTART, 172
        .equ    RUNEND, 176
        .equ    TOPF, 180
        .equ    SY0, 182
        .equ    SY1, 184
        .equ    SY0P, 186
        .equ    SYB, 188
        .equ    SLINE, 190
        .equ    CSTEP, 192              | columns: every CSTEP-th from CFIRST
        .equ    CFIRST, 194
        .equ    NPROJ, 196
        .equ    NRUN, 200
        .equ    RDX, 204
        .equ    RDZ, 206
        .equ    CAMXL, 208
        .equ    CAMZL, 212
        .equ    COARSE, 216
        .equ    SBUF, 220
        .equ    REC, 604                | 220 + 8 * 48
        .equ    GAPS0, 1244             | REC + 10 * 64
        .equ    GAPS, 1250

        .equ    RUNBYTES, 1440          | R_MAXN * 12
        .equ    NGMAX, 39               | R_MAXG - 1
        .equ    DSPLIT, 513
        .equ    NONE, -32768
        .equ    FROOM, 360              | room the fast loop needs on the bottom stack (30 runs)
        .equ    FMAXC, 8                | segments it collects at most (+ the last: 27 + 2 runs)
        .equ    TURF, 1
        .equ    LOG, 5
        .equ    LEAVES, 7
        .equ    TIDO, 112               | the type id table, after either height table (render.c)
        .equ    KTOP, 0x1000
        .equ    KBOT, 0x2000
        .equ    NFAR, 8                 | distant hills: samples, spacing (4 blocks)
        .equ    FARSTEP, 1024
        .equ    FARHILL, 12
        .equ    FARLINE, 14
        .equ    CWX, 32                 | coarse map: 32 x 32 (4 x 4 blocks each)
        .equ    SEAH, 28                | outside the world: the sea (SEA + 1)

cast_columns:
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        movea.l 48(%sp),%a4
        move.l  %sp,SAVESP(%a4)         | the fast loop's stack level (ffull returns to it)
        movea.l RUNS(%a4),%a5
        move.w  CFIRST(%a4),%d0
        move.w  %d0,COL(%a4)
        mulu.w  #RUNBYTES,%d0
        adda.l  %d0,%a5

| ======================================================================= column loop
colloop:
        | ---- ray direction: rd = dir + (pl * camx) >> 14
        movea.l CAMX(%a4),%a0
        move.w  COL(%a4),%d0
        add.w   %d0,%d0
        move.w  0(%a0,%d0.w),%d0
        move.w  PLX(%a4),%d1
        muls.w  %d0,%d1
        lsl.l   #2,%d1
        swap    %d1
        add.w   DIRX(%a4),%d1           | rdx
        move.w  PLZ(%a4),%d5
        muls.w  %d0,%d5
        lsl.l   #2,%d5
        swap    %d5
        add.w   DIRZ(%a4),%d5           | rdz
        move.w  %d1,RDX(%a4)
        move.w  %d5,RDZ(%a4)
        | ---- x: ddx (d6), sdx (d4), x step (a2), kx (d2), sx (d1.w)
        move.w  %d1,%d3
        bpl.s   1f
        neg.w   %d3
1:      move.w  #0x7FFF,%d6
        cmp.w   #128,%d3
        blt.s   2f
        move.l  #0x400000,%d0
        divu.w  %d3,%d0
        cmp.w   #0x7FFF,%d0
        bhi.s   2f
        move.w  %d0,%d6
2:      move.w  FX(%a4),%d0
        move.w  KX0(%a4),%d2
        tst.w   %d1
        ble.s   3f
        neg.w   %d0
        add.w   #256,%d0
        mulu.w  %d6,%d0
        lsr.l   #8,%d0
        move.w  %d0,%d4
        movea.l PWX4(%a4),%a2
        moveq   #1,%d1
        bra.s   4f
3:      mulu.w  %d6,%d0
        lsr.l   #8,%d0
        move.w  %d0,%d4
        move.l  PWX4(%a4),%d0
        neg.l   %d0
        movea.l %d0,%a2
        addq.w  #1,%d2
        moveq   #-1,%d1
4:      | ---- z: ddz (d7), sdz (d5), z step (a3), kz (d3), sz (d1 high word)
        move.w  %d5,%d3
        bpl.s   1f
        neg.w   %d3
1:      move.w  #0x7FFF,%d7
        cmp.w   #128,%d3
        blt.s   2f
        move.l  #0x400000,%d0
        divu.w  %d3,%d0
        cmp.w   #0x7FFF,%d0
        bhi.s   2f
        move.w  %d0,%d7
2:      move.w  FZ(%a4),%d0
        move.w  KZ0(%a4),%d3
        swap    %d1
        tst.w   %d5
        ble.s   3f
        neg.w   %d0
        add.w   #256,%d0
        mulu.w  %d7,%d0
        lsr.l   #8,%d0
        move.w  %d0,%d5
        movea.w #4,%a3
        move.w  #1,%d1
        bra.s   4f
3:      mulu.w  %d7,%d0
        lsr.l   #8,%d0
        move.w  %d0,%d5
        movea.w #-4,%a3
        addq.w  #1,%d3
        move.w  #-1,%d1
4:      swap    %d1

| ---- A: DDA, one 10-byte record per cell: key of the entry face, Seg*, far distance, recip
        movea.l CP0(%a4),%a1
        lea     REC(%a4),%a0
        movea.l RECIP(%a4),%a6
        clr.w   (%a0)+                  | the eye's cell: no entry face
dda:    cmp.w   %d5,%d4
        bcc.s   ddz                     | sdx >= sdz: z step
        move.w  %d4,%d0
        cmp.w   DMAX(%a4),%d0
        bcc.s   ddlast
        move.l  (%a1),(%a0)+
        move.w  %d0,(%a0)+
        add.w   %d0,%d0
        move.w  0(%a6,%d0.w),(%a0)+
        adda.l  %a2,%a1
        add.w   %d6,%d4
        add.w   %d1,%d2
        move.w  %d2,(%a0)+
        bra.s   dda
ddz:    move.w  %d5,%d0
        cmp.w   DMAX(%a4),%d0
        bcc.s   ddlast
        move.l  (%a1),(%a0)+
        move.w  %d0,(%a0)+
        add.w   %d0,%d0
        move.w  0(%a6,%d0.w),(%a0)+
        adda.l  %a3,%a1
        add.w   %d7,%d5
        swap    %d1
        add.w   %d1,%d3
        swap    %d1
        move.w  %d3,(%a0)+
        bra.s   dda
ddlast: move.l  (%a1),(%a0)+
        move.w  DMAX(%a4),%d0
        move.w  %d0,(%a0)+
        add.w   %d0,%d0
        move.w  0(%a6,%d0.w),(%a0)+
        move.l  %a0,RECEND(%a4)
        move.w  #-1,6(%a0)              | end mark for the fast loop (df < 0)
        tst.w   MODE(%a4)
        bne     colend                  | timing experiment: DDA only

| ---- B: process the cells. Fast mode, one gap [0, H), empty stacks.
        move.l  %a5,TOPSP(%a4)
        lea     FROOM(%a5),%a3
        move.l  %a3,BOTLIM(%a4)
        lea     RUNBYTES(%a5),%a3       | the bottom stack (a3 in the fast loop)
        move.l  %a3,BOTSP(%a4)
        clr.w   SLOW(%a4)
        move.w  #1,NG(%a4)
        clr.w   GSTART(%a4)
        move.w  H(%a4),%d4              | GEND
        move.w  %d4,GEND(%a4)
        move.w  VAR0(%a4),%d7           | variant of the eye's cell, fog 0
        movea.l RECIP(%a4),%a1
        clr.w   REC-4(%a4)              | the eye's cell: near distance 0, recip[0]
        move.w  (%a1),REC-2(%a4)
        movea.l T16(%a4),%a1            | near cells first
        move.w  H0(%a4),%d5
        moveq   #-1,%d6
        lea     REC(%a4),%a0
        bra     floop

| ---- light. A segment's type byte b holds its block type (bits 0-4) and light class (bits 5-6);
| TIDO(a1, b * 2) (a1: either height table) is (b & 31) << 5 | the frame's dither for the class.
| SIDEID: \d = that entry for segment a2's side seen from the previous cell (record a0 - 10):
| class 3 if torch-lit, else 0 if its top is above the previous cell's first top (open sky),
| else 1 if that first segment is leaves or a log, else 2. Clobbers \a.
        .macro  SIDEID d, a
        moveq   #0,\d
        move.b  2(%a2),\d
        cmp.b   #0x60,\d
        bcc.s   93f                     | torch-lit
        movea.l -8(%a0),\a              | the previous cell's first segment
        move.b  (%a2),\d
        cmp.b   (\a),\d
        bhi.s   91f                     | open sky
        move.b  2(\a),\d
        and.b   #31,\d
        cmp.b   #LEAVES,\d
        beq.s   92f
        cmp.b   #LOG,\d
        beq.s   92f
        movea.w #0x40,\a                | covered
        bra.s   94f
92:     movea.w #0x20,\a                | under a tree
        bra.s   94f
91:     suba.l  \a,\a
94:     moveq   #31,\d
        and.b   2(%a2),\d
        add.w   \a,\d
93:     add.w   \d,\d
        move.w  TIDO(%a1,\d\().w),\d
        .endm

| ======================================================================= fast loop
| Registers: a0 record, a1 this cell's far height table (T16, or T8 once df >= DSPLIT),
| a6 the near one (the previous cell's far table), a2 segment, a3 bottom stack, d4 GEND,
| d5 H0, d6 the previous cell's far projection (height * 2 in the high word, y; -1: none),
| d7 VF. Cells done here:
|   hidden:   the cell's topmost row (its first segment's top: at df when below the eye, at
|             dn when above) at or below GEND (any mode);
|   top face: the first segment's top below the eye, its side hidden (any mode);
|   sides:    the first run's sides contiguous from its top down to GEND (a step up, a hill,
|             a tree trunk), then the first segment's top face if below the eye (fast mode).
| Anything else goes the general way (fexit) before anything is painted.
floop:  move.w  6(%a0),%d1              | df (negative: past the last record)
        bmi     fend
        movea.l %a1,%a6                 | near table
        cmp.w   #DSPLIT,%d1
        bcs.s   1f
        movea.l T8(%a4),%a1
1:      movea.l 2(%a0),%a2
        moveq   #0,%d0
        move.b  (%a2),%d0
        add.w   %d0,%d0                 | top * 2
        move.w  0(%a1,%d0.w),%d2
        bge     fabove                  | top at or above the eye
        muls.w  8(%a0),%d2
        swap    %d2
        neg.w   %d2
        add.w   %d5,%d2                 | yf
        cmp.w   %d4,%d2
        bge     fdone                   | hidden
        swap    %d6                     | yn: cached, or projected at dn
        cmp.w   %d6,%d0
        bne.s   2f
        swap    %d6
        move.w  %d6,%d3
        bra.s   3f
2:      swap    %d6
        move.w  0(%a6,%d0.w),%d3
        muls.w  -2(%a0),%d3
        swap    %d3
        neg.w   %d3
        add.w   %d5,%d3                 | yn
3:      cmp.w   %d4,%d3
        blt     fstep                   | its side shows
        tst.w   SLOW(%a4)
        bne     fslow
        cmpa.l  BOTLIM(%a4),%a3
        bcs     fexit                   | out of room (never seen)
        bsr     ftop                    | the top face alone
fdone:  move.w  %d0,%d6                 | cache (height * 2, yf) for the next cell
        swap    %d6
        move.w  %d2,%d6
fnext:  eori.w  #16,%d7                 | neighbouring cells alternate variants
        cmp.w   FOGD(%a4),%d1
        blt.s   1f
        or.w    #1,%d7                  | fogged from the next cell on
1:      lea     10(%a0),%a0
        bra     floop
fhid:   moveq   #-1,%d6
        bra.s   fnext

| ---- the first segment's top at or above the eye: its side's top at dn is the topmost row
fabove: tst.w   (%a0)
        beq     fexit                   | the eye's cell: no sides
        move.w  0(%a6,%d0.w),%d3
        muls.w  -2(%a0),%d3
        swap    %d3
        neg.w   %d3
        add.w   %d5,%d3
        cmp.w   %d4,%d3
        bge.s   fhid                    | hidden
        tst.w   SLOW(%a4)
        bne     fexit
        moveq   #0,%d1                  | no top face; nothing collected
        bra.s   fsides
| ---- the first segment's top below the eye, its side showing (a step up)
fstep:  tst.w   (%a0)
        beq     fexit                   | the eye's cell (never seen)
        tst.w   SLOW(%a4)
        bne     fexit
        move.w  %d2,-(%sp)              | yf
        move.w  %d0,-(%sp)              | top * 2
        move.l  #0x10000,%d1            | a top face to do; nothing collected
| ---- the run's sides from a2 (its top on row d3 at dn) down to GEND: collect (Seg*, y0) on the
| stack top-down until one reaches GEND, then paint bottom-up. Not contiguous: the general way.
fsides: moveq   #0,%d0
        move.b  1(%a2),%d0
        add.w   %d0,%d0
        move.w  0(%a6,%d0.w),%d2
        muls.w  -2(%a0),%d2
        swap    %d2
        neg.w   %d2
        add.w   %d5,%d2                 | its bottom at dn
        cmp.w   %d4,%d2
        bge.s   femit                   | reaches GEND
        btst    #1,3(%a2)
        bne.s   fcolx                   | air below: not contiguous
        cmp.w   #FMAXC,%d1
        bcc.s   fcolx                   | (a very tall wall)
        move.l  %a2,-(%sp)
        move.w  %d3,-(%sp)
        addq.w  #1,%d1
        move.w  %d2,%d3
        addq.l  #4,%a2
        tst.b   (%a2)
        bne.s   fsides
fcolx:  move.w  %d1,%d0                 | drop what was collected (6 bytes each)
        add.w   %d0,%d0
        add.w   %d1,%d0
        add.w   %d0,%d0
        adda.w  %d0,%sp
        swap    %d1
        tst.w   %d1
        beq     fexit
        addq.l  #4,%sp                  | and the top face's (top, yf)
        bra     fexit
femit:  cmpa.l  BOTLIM(%a4),%a3
        bcs.s   fcolx                   | out of room (never seen)
        moveq   #0,%d0                  | the side key: the entry face's, 0 when fogged
        btst    #0,%d7
        bne.s   1f
        move.w  (%a0),%d0
1:      move.w  %d0,PKEY(%a4)
        move.w  %d7,%d0
        addq.w  #4,%d0
        btst    #6,(%a0)                | a z face (key 0x4000 + ...)?
        beq.s   2f
        addq.w  #4,%d0
2:      move.w  %d0,SFACE(%a4)          | VF + 4 (x face) or VF + 8 (z face)
| one segment (a2, top on row d3): line [y0, y0 + 1) if its top is above the eye, turf band
| [y0', yb), side [yb, GEND); pushed bottom-up. y0 < GEND here.
fseg:   SIDEID  %d0,%a6                 | its id and light
        move.w  %d0,SSH(%a4)
        moveq   #0,%d0
        move.b  (%a2),%d0
        add.w   %d0,%d0                 | top * 2
        move.w  %d3,%d2                 | y0'
        tst.w   0(%a1,%d0.w)
        ble.s   1f
        addq.w  #1,%d2                  | an edge line on row y0 first
1:      moveq   #31,%d0
        and.b   2(%a2),%d0
        subq.b  #TURF,%d0
        bne.s   fside
        move.b  (%a2),%d0
        add.w   %d0,%d0                 | top * 2
        movea.l B8(%a4),%a6             | the grass band: a quarter block under the top
        cmp.w   #DSPLIT,-4(%a0)
        bcc.s   2f
        movea.l B16(%a4),%a6
2:      move.w  0(%a6,%d0.w),%d0
        muls.w  -2(%a0),%d0
        swap    %d0
        neg.w   %d0
        add.w   %d5,%d0                 | yb
        cmp.w   %d2,%d0
        bge.s   3f
        move.w  %d2,%d0
3:      cmp.w   %d4,%d0
        bge.s   4f                      | no side rows
        bsr     fpside                  | the side [yb, GEND)
4:      cmp.w   %d4,%d2
        bge.s   fline
        lea     -12(%a3),%a3            | the band [y0', GEND)
        move.w  %d4,(%a3)
        moveq   #3,%d0
        and.w   SSH(%a4),%d0
        or.w    %d7,%d0
        addq.w  #8,%d0
        move.w  %d0,2(%a3)
        move.w  PKEY(%a4),4(%a3)
        move.w  -4(%a0),6(%a3)
        move.w  %d2,%d4
        cmp.w   GSTART(%a4),%d4
        ble     ffull
        bra.s   fline
fside:  move.w  %d2,%d0
        cmp.w   %d4,%d0
        bge.s   fline
        bsr     fpside                  | the side [y0', GEND)
fline:  cmp.w   %d3,%d2
        beq.s   fsnext                  | no line
        cmp.w   %d4,%d3
        bge.s   fsnext
        lea     -12(%a3),%a3            | the line [y0, GEND)
        move.w  %d4,(%a3)
        move.w  SSH(%a4),%d0
        or.w    %d7,%d0
        and.w   #3,%d0
        addq.w  #4,%d0
        move.w  %d0,2(%a3)
        move.w  PKEY(%a4),4(%a3)
        move.w  -4(%a0),6(%a3)
        move.w  %d3,%d4
        cmp.w   GSTART(%a4),%d4
        ble     ffull
fsnext: tst.w   %d1
        beq.s   fsdone
        subq.w  #1,%d1
        move.w  (%sp)+,%d3
        movea.l (%sp)+,%a2
        bra     fseg
fsdone: move.w  6(%a0),%d2              | (df for fnext below)
        swap    %d1
        tst.w   %d1
        bne.s   1f
        move.w  %d2,%d1
        bra     fhid                    | no top face: no cache
1:      move.w  %d2,%d1                 | df
        move.w  (%sp)+,%d0              | top * 2
        move.w  (%sp)+,%d2              | yf
        bsr     ftop                    | the top face over yn = d3
        bra     fdone
| fpside: push the side of segment a2 (its id and light in SSH) on [d0, GEND).
fpside: lea     -12(%a3),%a3
        move.w  %d4,(%a3)
        move.w  %d0,%d4
        move.w  SSH(%a4),%d0
        or.w    SFACE(%a4),%d0
        move.w  %d0,2(%a3)
        move.w  PKEY(%a4),4(%a3)
        move.w  -4(%a0),6(%a3)
        cmp.w   GSTART(%a4),%d4
        ble     ffull
        rts

| ftop: the top face of segment a2 on [yf (d2), GEND), with an edge line on row yf if yn - yf
| >= 3 (yn in d3); key KTOP | height * 2 (d0) unless fogged, depth df (d1). Fast mode.
| Keeps d0-d2, a2. Clobbers d3, a6.
ftop:   cmp.w   %d4,%d2
        bge.s   9f                      | empty
        suba.l  %a6,%a6                 | key: 0 when fogged
        btst    #0,%d7
        bne.s   1f
        movea.w %d0,%a6
        adda.w  #KTOP,%a6
1:      sub.w   %d2,%d3
        cmp.w   #3,%d3
        blt.s   4f
        moveq   #0,%d3                  | with the line: the face [yf + 1, GEND) unless empty
        move.b  2(%a2),%d3
        add.w   %d3,%d3
        move.w  TIDO(%a1,%d3.w),%d3
        or.w    %d7,%d3
        addq.w  #1,%d2
        cmp.w   %d2,%d4
        ble.s   2f
        lea     -12(%a3),%a3
        move.w  %d4,(%a3)
        move.w  %d3,2(%a3)
        move.w  %a6,4(%a3)
        move.w  %d1,6(%a3)
        move.w  %d2,%d4
        cmp.w   GSTART(%a4),%d4
        ble     ffull
2:      subq.w  #1,%d2
        and.w   #3,%d3                  | then the line [yf, yf + 1)
        addq.w  #4,%d3
        bra.s   5f
4:      moveq   #0,%d3                  | the face [yf, GEND)
        move.b  2(%a2),%d3
        add.w   %d3,%d3
        move.w  TIDO(%a1,%d3.w),%d3
        or.w    %d7,%d3
5:      lea     -12(%a3),%a3
        move.w  %d4,(%a3)
        move.w  %d3,2(%a3)
        move.w  %a6,4(%a3)
        move.w  %d1,6(%a3)
        move.w  %d2,%d4
        cmp.w   GSTART(%a4),%d4
        ble     ffull
9:      rts

| ---- the top face alone in slow mode (the general gap list): new runs at NN after the last
| gap's run n, which now ends at yf. (a3 is free: the bottom stack is unused in slow mode.)
fslow:  sub.w   %d2,%d3                 | yn - yf
        cmp.w   #NGMAX,NG(%a4)
        bge     fexit
        movea.l LASTG(%a4),%a3
        cmp.w   (%a3),%d2
        ble     fexit                   | would fill the last gap
        move.w  NNLIM(%a4),%d1
        cmp.w   NN(%a4),%d1
        blt     fexit                   | out of runs (never seen)
        move.w  6(%a0),%d1              | df
        move.w  %d2,2(%a3)              | the last gap now ends at yf
        move.w  4(%a3),%d6              | n
        movea.l %a5,%a3
        adda.w  %d6,%a3                 | a3 = run n
        movea.l %a5,%a6
        adda.w  NN(%a4),%a6             | a6 = the first new run
        move.w  8(%a3),-(%sp)           | n.next
        move.w  NN(%a4),8(%a3)          | n.next = the first new run
        move.w  %d2,(%a3)               | n ends at yf
        moveq   #0,%d6
        btst    #0,%d7
        bne.s   1f
        move.w  %d0,%d6
        or.w    #KTOP,%d6               | key
1:      cmp.w   #3,%d3
        blt.s   4f
        move.w  %d2,%d3                 | the line [yf, yf + 1)
        addq.w  #1,%d3
        move.w  %d3,(%a6)
        moveq   #0,%d3
        move.b  2(%a2),%d3
        add.w   %d3,%d3
        move.w  TIDO(%a1,%d3.w),%d3
        or.w    %d7,%d3
        and.w   #3,%d3
        addq.w  #4,%d3
        move.w  %d3,2(%a6)
        move.w  %d6,4(%a6)
        move.w  %d1,6(%a6)
        add.w   #12,NN(%a4)
        move.w  (%a6),%d3
        cmp.w   %d4,%d3
        blt.s   3f
        move.w  (%sp)+,8(%a6)           | the line alone: line.next = n.next
        bra.s   6f
3:      move.w  NN(%a4),8(%a6)          | line.next = the face
        lea     12(%a6),%a6
4:      move.w  %d4,(%a6)               | the face [.., GEND)
        moveq   #0,%d3
        move.b  2(%a2),%d3
        add.w   %d3,%d3
        move.w  TIDO(%a1,%d3.w),%d3
        or.w    %d7,%d3
        move.w  %d3,2(%a6)
        move.w  %d6,4(%a6)
        move.w  %d1,6(%a6)
        move.w  (%sp)+,8(%a6)           | face.next = n.next
        add.w   #12,NN(%a4)
6:      move.w  %d2,%d4                 | GEND = yf
        move.w  %d2,GEND(%a4)
        bra     fdone

| ---- this cell the general way: spill the fast loop's state
fexit:  bra     gcell
fend:   move.w  %d4,GEND(%a4)
        move.l  %a3,BOTSP(%a4)
        bra     colfin

ffull:  | the column is full: everything else is hidden
        movea.l SAVESP(%a4),%sp
        clr.w   NG(%a4)
        move.w  #32767,GSTART(%a4)
        move.w  #-32768,GEND(%a4)
        move.l  %a3,BOTSP(%a4)
        bra     colfin

| ======================================================================= general cells
| Any cell: its pieces top-down into a sorted list without overlaps (a run's bottom face and its
| line give way to the pieces after them; anything else wins over later pieces), then painted.
| Fast mode: the tail attached to GEND goes on the bottom stack, the head attached to GSTART on
| the top stack, the rest through paint. Slow mode: all through paint.
| A piece: y0, y1, id (bit 15: gives way), key, depth (10 bytes, stored by one movem.w d0-d4).
| PBUF[0] is a sentinel (y1 = -32768). Registers: a0 record, a1 far table, a6 near table,
| a2 segment, a3 the list's end, a5 the chain (the last side's y1 at dn; NONE: none; the runs
| pointer is on the stack), d5 H0, d6 the previous cell's far projection, d7 VF; d0-d4 the
| piece being added (y0, y1, id, key, depth).

| PADD: add the piece d0-d4 after the list's last one (see gover)
        .macro  PADD
        cmp.w   -8(%a3),%d0
        blt.s   8f
7:      cmp.w   %d1,%d0
        bge.s   9f                      | empty
        cmp.w   GSTART(%a4),%d1
        ble.s   9f                      | above every gap
        movem.w %d0-%d4,(%a3)
        lea     10(%a3),%a3
        bra.s   9f
8:      bsr     gover
        bra.s   7b
9:
        .endm

gcell:  move.w  %d4,GEND(%a4)
        move.l  %a3,BOTSP(%a4)
        move.l  %a5,-(%sp)
        clr.w   NCH(%a4)                | this cell's far projection: none yet
        moveq   #0,%d0                  | the side key (0 when fogged) and face offset
        btst    #0,%d7
        bne.s   1f
        move.w  (%a0),%d0
1:      move.w  %d0,SKEY(%a4)
        move.w  %d7,%d0
        addq.w  #4,%d0
        btst    #6,(%a0)
        beq.s   2f
        addq.w  #4,%d0
2:      move.w  %d0,SFACE(%a4)          | VF + 4 (x face) or VF + 8 (z face)
        movea.l PBUF(%a4),%a3
        move.w  #NONE,2(%a3)            | the sentinel
        lea     10(%a3),%a3
        movea.l 2(%a0),%a2
| ---- one run: its top face (top below the eye), sides, bottom face (bottom above the eye)
grun:   moveq   #0,%d0
        move.b  (%a2),%d0
        beq     gpaint                  | the end of the list
        moveq   #0,%d1
        move.b  3(%a2),%d1
        lsr.b   #2,%d1                  | segments in this run
        lsl.w   #2,%d1
        lea     0(%a2,%d1.w),%a5
        move.l  %a5,RUNEND(%a4)
        movea.w #NONE,%a5               | no chain yet
        add.w   %d0,%d0                 | top * 2
        move.w  0(%a1,%d0.w),%d2
        bge     gsides                  | top at or above the eye: no top face
        muls.w  8(%a0),%d2
        swap    %d2
        neg.w   %d2
        add.w   %d5,%d2                 | yf
        tst.w   NCH(%a4)
        bne.s   1f
        move.w  #1,NCH(%a4)
        move.w  %d0,CH0(%a4)
        move.w  %d2,CY0(%a4)
1:      cmp.w   GEND(%a4),%d2
        bge     gpaint                  | hidden from here down
        swap    %d6                     | yn: cached, or projected at dn
        cmp.w   %d6,%d0
        bne.s   2f
        swap    %d6
        move.w  %d6,%d1
        bra.s   3f
2:      swap    %d6
        move.w  0(%a6,%d0.w),%d1
        muls.w  -2(%a0),%d1
        swap    %d1
        neg.w   %d1
        add.w   %d5,%d1                 | yn
3:      movea.w %d1,%a5                 | the chain
        moveq   #0,%d3
        btst    #0,%d7
        bne.s   4f
        move.w  %d0,%d3
        or.w    #KTOP,%d3               | key
4:      move.w  6(%a0),%d4              | depth: df
        move.w  %d2,%d0                 | yf
        sub.w   %d0,%d1
        cmp.w   #3,%d1
        blt.s   5f
        move.w  %d0,%d1                 | the line [yf, yf + 1)
        addq.w  #1,%d1
        moveq   #0,%d2
        move.b  2(%a2),%d2
        add.w   %d2,%d2
        move.w  TIDO(%a1,%d2.w),%d2
        or.w    %d7,%d2
        and.w   #3,%d2
        addq.w  #4,%d2
        PADD
        move.w  %d1,%d0                 | the face [yf + 1, yn)
5:      move.w  %a5,%d1                 | (or [yf, yn))
        moveq   #0,%d2
        move.b  2(%a2),%d2
        add.w   %d2,%d2
        move.w  TIDO(%a1,%d2.w),%d2
        or.w    %d7,%d2
        PADD
gsides: tst.w   (%a0)
        beq     gbot                    | the eye's cell: no sides
        move.w  -4(%a0),%d4             | depth: dn
        move.w  SKEY(%a4),%d3
gsl:    cmpa.l  RUNEND(%a4),%a2
        bcc     gbot
        moveq   #0,%d2
        move.b  (%a2),%d2
        add.w   %d2,%d2                 | top * 2
        move.w  %a5,%d0                 | y0: the chain, or Y(top, dn)
        cmp.w   #NONE,%d0
        bne.s   1f
        move.w  0(%a6,%d2.w),%d0
        muls.w  -2(%a0),%d0
        swap    %d0
        neg.w   %d0
        add.w   %d5,%d0
1:      cmp.w   GEND(%a4),%d0
        bge     gpaint                  | hidden from here down
        SIDEID  %d2,%a5                 | its id and light (the chain is in d0 now)
        move.w  %d2,SSH(%a4)
        moveq   #0,%d2
        move.b  (%a2),%d2
        add.w   %d2,%d2
        tst.w   0(%a1,%d2.w)
        ble.s   2f
        move.w  %d0,%d1                 | top above the eye: the edge line [y0, y0 + 1)
        addq.w  #1,%d1
        move.w  SSH(%a4),%d2
        or.w    %d7,%d2
        and.w   #3,%d2
        addq.w  #4,%d2
        PADD
        move.w  %d1,%d0                 | y0'
2:      moveq   #0,%d2                  | y1 = Y(bot, dn): the chain
        move.b  1(%a2),%d2
        add.w   %d2,%d2
        move.w  0(%a6,%d2.w),%d1
        muls.w  -2(%a0),%d1
        swap    %d1
        neg.w   %d1
        add.w   %d5,%d1
        movea.w %d1,%a5
        moveq   #31,%d2
        and.b   2(%a2),%d2
        subq.b  #TURF,%d2
        bne.s   4f
        moveq   #0,%d2                  | the grass band [y0', yb), yb = max(Y(top - 1/4), y0')
        move.b  (%a2),%d2
        add.w   %d2,%d2
        move.l  %a5,-(%sp)
        movea.l B8(%a4),%a5
        cmp.w   #DSPLIT,-4(%a0)
        bcc.s   3f
        movea.l B16(%a4),%a5
3:      move.w  0(%a5,%d2.w),%d1
        movea.l (%sp)+,%a5
        muls.w  -2(%a0),%d1
        swap    %d1
        neg.w   %d1
        add.w   %d5,%d1                 | yb
        cmp.w   %d0,%d1
        bge.s   31f
        move.w  %d0,%d1
31:     moveq   #3,%d2
        and.w   SSH(%a4),%d2
        or.w    %d7,%d2
        addq.w  #8,%d2
        PADD
        move.w  %d1,%d0
4:      move.w  %a5,%d1                 | the side [y0', y1)
        move.w  SSH(%a4),%d2
        or.w    SFACE(%a4),%d2
        PADD
        addq.l  #4,%a2
        bra     gsl
gbot:   movea.l RUNEND(%a4),%a2         | the run's last segment is at a2 - 4
        btst    #1,-1(%a2)
        beq     grun                    | no air below
        moveq   #0,%d2
        move.b  -3(%a2),%d2
        add.w   %d2,%d2                 | bot * 2
        tst.w   0(%a1,%d2.w)
        ble     grun                    | bottom at or below the eye
        move.w  %a5,%d0                 | yn: the chain, or Y(bot, dn)
        cmp.w   #NONE,%d0
        bne.s   1f
        move.w  0(%a6,%d2.w),%d0
        muls.w  -2(%a0),%d0
        swap    %d0
        neg.w   %d0
        add.w   %d5,%d0
1:      move.w  0(%a1,%d2.w),%d1
        muls.w  8(%a0),%d1
        swap    %d1
        neg.w   %d1
        add.w   %d5,%d1                 | yf = Y(bot, df)
        tst.w   NCH(%a4)
        bne.s   2f
        move.w  #1,NCH(%a4)
        move.w  %d2,CH0(%a4)
        move.w  %d1,CY0(%a4)
2:      cmp.w   GEND(%a4),%d0
        bge     gpaint                  | hidden from here down
        moveq   #0,%d3
        btst    #0,%d7
        bne.s   3f
        move.w  %d2,%d3
        or.w    #KBOT,%d3               | key
3:      move.w  6(%a0),%d4              | depth: df
        moveq   #0x60,%d2               | the light below it: the next segment's class
        and.b   2(%a2),%d2
        movea.w %d2,%a5
        moveq   #31,%d2
        and.b   -2(%a2),%d2
        add.w   %a5,%d2
        add.w   %d2,%d2
        move.w  TIDO(%a1,%d2.w),%d2
        or.w    %d7,%d2
        add.w   #0x800C,%d2             | the bottom face (+ 12), gives way
        movea.w %d1,%a5                 | yf (the chain is no longer needed)
        sub.w   %d0,%d1
        cmp.w   #3,%d1
        blt.s   4f
        move.w  %a5,%d1
        subq.w  #1,%d1
        PADD                            | the face [yn, yf - 1)
        move.w  %d1,%d0
        move.w  %a5,%d1
        and.w   #3,%d2
        add.w   #0x8004,%d2
        PADD                            | its line [yf - 1, yf)
        bra     grun
4:      move.w  %a5,%d1
        PADD                            | the face [yn, yf)
        bra     grun

| gover: the new piece [d0, ..) overlaps the last piece: that one gives way (ends at d0; gone
| if empty, then the one before is looked at), or the new piece starts after it.
gover:  tst.w   -6(%a3)
        bpl.s   2f
        move.w  %d0,-8(%a3)
        cmp.w   -10(%a3),%d0
        bgt.s   1f
        lea     -10(%a3),%a3
        cmp.w   -8(%a3),%d0
        blt.s   gover
1:      rts
2:      move.w  -8(%a3),%d0
        rts

| ---- paint the list
gpaint: movea.l (%sp)+,%a5
        movea.l PBUF(%a4),%a2
        lea     10(%a2),%a2             | the first piece
        tst.w   SLOW(%a4)
        bne     gb
        move.w  GEND(%a4),%d4           | fast mode: the tail attached to GEND, bottom-up
        movea.l BOTSP(%a4),%a6
gfb:    cmpa.l  %a2,%a3
        bls.s   gfbx
        cmp.w   -8(%a3),%d4
        bgt.s   gfbx                    | not attached
        lea     -10(%a3),%a3
        move.w  (%a3),%d0
        cmp.w   %d4,%d0
        bge.s   gfb                     | hidden
        lea     -12(%a6),%a6
        cmpa.l  TOPSP(%a4),%a6
        bcs.s   gfbr                    | out of room (never seen)
        move.w  %d4,(%a6)
        move.w  4(%a3),%d1
        and.w   #0x7FFF,%d1
        move.w  %d1,2(%a6)
        move.l  6(%a3),4(%a6)
        move.w  %d0,%d4
        cmp.w   GSTART(%a4),%d4
        bgt.s   gfb
        move.l  %a6,BOTSP(%a4)          | the column is full
        clr.w   NG(%a4)
        move.w  #32767,GSTART(%a4)
        move.w  #-32768,GEND(%a4)
        movea.l SAVESP(%a4),%sp
        bra     colfin
gfbr:   lea     12(%a6),%a6
        lea     10(%a3),%a3
gfbx:   move.w  %d4,GEND(%a4)
        move.l  %a6,BOTSP(%a4)
        move.w  GSTART(%a4),%d4         | the head attached to GSTART: top stack, top-down
        movea.l TOPSP(%a4),%a6
gft:    cmpa.l  %a2,%a3
        bls.s   gftx
        cmp.w   (%a2),%d4
        blt.s   gftx                    | not attached
        cmp.w   2(%a2),%d4
        bge.s   gftn                    | above every gap (a piece that gave way)
        lea     24(%a6),%a6
        cmpa.l  BOTSP(%a4),%a6
        bhi.s   gftr                    | out of room (never seen)
        lea     -24(%a6),%a6
        move.w  2(%a2),%d1
        move.w  %d1,(%a6)
        move.w  4(%a2),%d0
        and.w   #0x7FFF,%d0
        move.w  %d0,2(%a6)
        move.l  6(%a2),4(%a6)
        lea     12(%a6),%a6
        move.w  %d1,%d4
gftn:   lea     10(%a2),%a2
        bra.s   gft
gftr:   lea     -24(%a6),%a6
gftx:   move.w  %d4,GSTART(%a4)
        move.l  %a6,TOPSP(%a4)
        lea     FROOM(%a6),%a6
        move.l  %a6,BOTLIM(%a4)
        bra.s   gm
gb:     cmpa.l  %a2,%a3                 | slow mode: the tail attached to the last gap's end
        bls.s   gdone
        move.w  -8(%a3),%d1
        cmp.w   GEND(%a4),%d1
        blt.s   gm
        lea     -10(%a3),%a3
        movea.l %a3,%a6
        bsr.s   gpp
        bra.s   gb
gm:     cmpa.l  %a2,%a3                 | the rest
        bls.s   gdone
        movea.l %a2,%a6
        bsr.s   gpp
        lea     10(%a2),%a2
        bra.s   gm
gdone:  moveq   #-1,%d6                 | this cell's far projection for the next cell
        tst.w   NCH(%a4)
        beq.s   1f
        move.w  CH0(%a4),%d6
        swap    %d6
        move.w  CY0(%a4),%d6
1:      move.w  GEND(%a4),%d4
        movea.l BOTSP(%a4),%a3
        move.w  6(%a0),%d1
        bra     fnext
| gpp: paint the piece at a6; the column full: done with it
gpp:    move.w  4(%a6),%d0
        and.w   #0x7FFF,%d0
        move.w  %d0,PID(%a4)
        move.w  6(%a6),PKEY(%a4)
        move.w  8(%a6),DEPTH(%a4)
        move.w  (%a6),%d0
        move.w  2(%a6),%d1
        bsr     paint
        tst.w   NG(%a4)
        beq.s   1f
        rts
1:      movea.l SAVESP(%a4),%sp
        bra     colfin

| ---- end of the column: distant hills, then link the fast-mode stacks into one run list
colfin: tst.w   NG(%a4)
        beq     colfin2
        bsr     farhills
colfin2:
        moveq   #0,%d1                  | start offset
        tst.w   SLOW(%a4)
        bne.s   colstart
        bsr     linkfast
colstart:
        movea.l RSTART(%a4),%a6
        move.w  COL(%a4),%d0
        add.w   %d0,%d0
        move.w  %d1,0(%a6,%d0.w)
colend: move.w  CSTEP(%a4),%d0
        add.w   %d0,COL(%a4)
        mulu.w  #RUNBYTES,%d0
        adda.l  %d0,%a5
        move.w  COL(%a4),%d0
        cmp.w   NCOLS(%a4),%d0
        blt     colloop
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

| linkfast: top stack entries -> (gap run if not full) -> bottom stack entries, linked by
| byte offsets from the column's start; d1 = offset of the first run. Clobbers d0-d3, a6.
| With the gap run written, also sets up the general gap list (used by pconvert).
linkfast:
        move.l  TOPSP(%a4),%d2
        sub.l   %a5,%d2                 | top stack bytes (= the gap run's offset)
        moveq   #12,%d0
        moveq   #12,%d1
        cmp.w   %d2,%d0
        bgt.s   2f
1:      move.w  %d0,-4(%a5,%d0.w)       | entry at d0 - 12: next = d0
        add.w   %d1,%d0
        cmp.w   %d2,%d0
        ble.s   1b
2:      move.l  BOTSP(%a4),%d3
        sub.l   %a5,%d3                 | bottom stack start offset
        moveq   #0,%d1
        tst.w   NG(%a4)
        beq.s   3f
        | the gap run at the top stack's end: [GSTART, GEND), next = the bottom stack
        movea.l TOPSP(%a4),%a6
        move.w  GEND(%a4),(%a6)
        move.w  #-1,2(%a6)
        clr.w   4(%a6)
        move.w  #0x7FFF,6(%a6)
        move.w  %d3,8(%a6)
        bra.s   4f
3:      | full: the last top entry links straight to the bottom stack
        tst.w   %d2
        bne.s   5f
        move.w  %d3,%d1                 | no top entries: the list starts in the bottom stack
        bra.s   4f
5:      move.w  %d3,-4(%a5,%d2.w)
4:      | bottom stack: next = offset + 12
        move.w  %d3,%d0
        add.w   #12,%d0
        moveq   #12,%d2
        move.w  #RUNBYTES,%d3
        cmp.w   %d3,%d0
        bgt.s   7f
6:      move.w  %d0,-4(%a5,%d0.w)
        add.w   %d2,%d0
        cmp.w   %d3,%d0
        ble.s   6b
7:      rts

| ======================================================================= helpers
| farhills: NFAR samples of the coarse height map beyond the view distance along this column's
| ray (render.c far_hills); each paints the hill [y + 1, H) then its line [y, y + 1) (the same
| result as the line first). Registers: d4/d5 sample x/z (8.8, long), a2/a3 their steps,
| d6 distance, d7 k, a1 y. Clobbers d0-d7, a1-a3, a6.
farhills:
        move.w  RDX(%a4),%d0
        move.w  %d0,%d1
        asr.w   #4,%d1
        movea.w %d1,%a2                 | x step: (rdx * FARSTEP) >> 14
        muls.w  DMAX(%a4),%d0
        lsl.l   #2,%d0
        swap    %d0
        ext.l   %d0
        move.l  CAMXL(%a4),%d4
        add.l   %d0,%d4                 | x at DMAX: camx + (rdx * DMAX) >> 14
        move.w  RDZ(%a4),%d0
        move.w  %d0,%d1
        asr.w   #4,%d1
        movea.w %d1,%a3
        muls.w  DMAX(%a4),%d0
        lsl.l   #2,%d0
        swap    %d0
        ext.l   %d0
        move.l  CAMZL(%a4),%d5
        add.l   %d0,%d5
        move.w  DMAX(%a4),%d6
        moveq   #1,%d7
hloop:  add.w   #FARSTEP,%d6            | d = DMAX + k * FARSTEP
        add.l   %a2,%d4
        add.l   %a3,%d5
        move.l  %d4,%d0
        asr.l   #8,%d0
        asr.l   #2,%d0                  | hx
        moveq   #CWX,%d2
        cmp.l   %d2,%d0
        bcc.s   1f                      | outside the world (unsigned)
        move.l  %d5,%d1
        asr.l   #8,%d1
        asr.l   #2,%d1                  | hz
        cmp.l   %d2,%d1
        bcc.s   1f
        lsl.w   #5,%d0
        add.w   %d1,%d0
        movea.l COARSE(%a4),%a6
        moveq   #0,%d2
        move.b  0(%a6,%d0.w),%d2
        bra.s   2f
1:      moveq   #SEAH,%d2
2:      add.w   %d2,%d2
        movea.l T8(%a4),%a6
        move.w  0(%a6,%d2.w),%d2        | (h << 8) - eye
        movea.l RECIP(%a4),%a6
        move.w  %d6,%d0
        add.w   %d0,%d0
        muls.w  0(%a6,%d0.w),%d2
        swap    %d2
        neg.w   %d2
        add.w   H0(%a4),%d2             | y
        cmp.w   GEND(%a4),%d2
        bge     3f                      | below every gap: hidden
        moveq   #FARHILL,%d0
        cmp.w   #NFAR/2,%d7
        ble.s   4f
        addq.w  #1,%d0
4:      tst.w   SLOW(%a4)
        bne.s   6f
        | fast mode: the hill [y + 1, GEND) and its line [y, y + 1) on the bottom stack
        movea.l BOTSP(%a4),%a6
        lea     -24(%a6),%a6
        cmpa.l  TOPSP(%a4),%a6
        bcs.s   6f                      | (out of room: the general way)
        lea     24(%a6),%a6
        move.w  GEND(%a4),%d1
        move.w  %d2,%d3
        addq.w  #1,%d3
        cmp.w   %d1,%d3
        bge.s   7f                      | no hill rows below the line
        lea     -12(%a6),%a6
        move.w  %d1,(%a6)
        move.w  %d0,2(%a6)
        clr.w   4(%a6)
        move.w  %d6,6(%a6)
        move.w  %d3,%d1
        cmp.w   GSTART(%a4),%d1
        ble.s   8f                      | full
7:      lea     -12(%a6),%a6
        move.w  %d1,(%a6)
        move.w  #FARLINE,2(%a6)
        clr.w   4(%a6)
        move.w  %d6,6(%a6)
        move.w  %d2,%d1
        move.l  %a6,BOTSP(%a4)
        move.w  %d1,GEND(%a4)
        cmp.w   GSTART(%a4),%d1
        bgt.s   3f
        bra.s   9f                      | full
8:      move.l  %a6,BOTSP(%a4)
9:      clr.w   NG(%a4)
        move.w  #32767,GSTART(%a4)
        move.w  #-32768,GEND(%a4)
        bra.s   5f
6:      movea.w %d2,%a1                 | slow mode: through paint
        move.w  %d6,DEPTH(%a4)
        clr.w   PKEY(%a4)
        move.w  %d0,PID(%a4)
        move.w  %a1,%d0
        addq.w  #1,%d0
        move.w  H(%a4),%d1
        bsr     paint                   | the hill [y + 1, H)
        move.w  #FARLINE,PID(%a4)
        move.w  %a1,%d0
        move.w  %d0,%d1
        addq.w  #1,%d1
        bsr     paint                   | its line [y, y + 1)
        tst.w   NG(%a4)
        beq.s   5f                      | the column is full
3:      addq.w  #1,%d7
        cmp.w   #NFAR,%d7
        ble     hloop
5:      rts

| ======================================================================= paint
| paint: rows [d0, d1) of this column get PID/PKEY/DEPTH where still uncovered.
paint:
        cmp.w   %d1,%d0
        bge     pret                    | empty
        tst.w   SLOW(%a4)
        bne     pslow
        | fast mode: one gap [GSTART, GEND)
        move.w  GSTART(%a4),%d2
        cmp.w   %d2,%d1
        ble     pret                    | ends above the gap
        move.w  GEND(%a4),%d3
        cmp.w   %d3,%d0
        bge     pret                    | starts below the gap
        cmp.w   %d3,%d1
        blt     ptopq
        | reaches the gap's bottom: push [max(y0, gs), ge) on the bottom stack
        movea.l BOTSP(%a4),%a6
        lea     -12(%a6),%a6
        move.l  %a6,%d3
        sub.l   TOPSP(%a4),%d3
        cmp.l   #12,%d3
        blt     pret                    | out of room (never seen)
        move.l  %a6,BOTSP(%a4)
        move.w  GEND(%a4),(%a6)
        move.w  PID(%a4),2(%a6)
        move.w  PKEY(%a4),4(%a6)
        move.w  DEPTH(%a4),6(%a6)
        cmp.w   %d2,%d0
        ble     pfull                   | covers the whole gap: the column is full
        move.w  %d0,GEND(%a4)
        rts
pfull:  clr.w   NG(%a4)
        move.w  #32767,GSTART(%a4)
        move.w  #-32768,GEND(%a4)
        rts
ptopq:  cmp.w   %d2,%d0
        bgt     pconvert                | strictly inside the gap
        | reaches the gap's top (not its bottom): push [gs, y1) on the top stack
        movea.l TOPSP(%a4),%a6
        move.l  BOTSP(%a4),%d3
        sub.l   %a6,%d3
        cmp.l   #24,%d3
        blt     pret                    | out of room (never seen)
        move.w  %d1,(%a6)
        move.w  PID(%a4),2(%a6)
        move.w  PKEY(%a4),4(%a6)
        move.w  DEPTH(%a4),6(%a6)
        lea     12(%a6),%a6
        move.l  %a6,TOPSP(%a4)
        move.w  %d1,GSTART(%a4)
        lea     FROOM(%a6),%a6
        move.l  %a6,BOTLIM(%a4)
        rts
| pconvert: switch this column to the general gap list, then paint generally
pconvert:
        movem.l %d0-%d1,-(%sp)
        bsr     linkfast                | writes the gap run at TOPSP and links everything
        movem.l (%sp)+,%d0-%d1
        move.l  TOPSP(%a4),%d2
        sub.l   %a5,%d2                 | the gap run's offset
        lea     GAPS(%a4),%a6
        move.l  %a6,LASTG(%a4)
        move.w  GSTART(%a4),(%a6)
        move.w  GEND(%a4),2(%a6)
        move.w  %d2,4(%a6)
        move.w  #32767,6(%a6)           | end sentinel
        move.w  #32767,8(%a6)
        add.w   #12,%d2
        move.w  %d2,NN(%a4)             | new runs after the gap run
        move.l  BOTSP(%a4),%d3
        sub.l   %a5,%d3
        sub.w   #24,%d3
        move.w  %d3,NNLIM(%a4)          | ... and below the bottom stack, two at a time
        move.w  #1,SLOW(%a4)
        bra     pgen
| ---- general mode
pslow:  tst.w   %d0
        bpl.s   1f
        moveq   #0,%d0
1:      cmp.w   H(%a4),%d1
        ble.s   2f
        move.w  H(%a4),%d1
2:      cmp.w   %d1,%d0
        bge     pret
        | the piece covers the bottom part of the last gap [a, b): a < y0 < b <= y1
        move.w  GEND(%a4),%d2
        cmp.w   %d2,%d1
        blt     pgen
        cmp.w   %d2,%d0
        bge     pret                    | below every gap
        movea.l LASTG(%a4),%a6
        cmp.w   (%a6),%d0
        ble     pgen
        move.w  NNLIM(%a4),%d3
        cmp.w   NN(%a4),%d3
        blt     pret
        cmp.w   #NGMAX,NG(%a4)
        bge     pret
        move.w  4(%a6),%d2              | n keeps [a, y0); new run m = [y0, b)
        move.w  NN(%a4),%d3
        move.w  GEND(%a4),0(%a5,%d3.w)
        move.w  PID(%a4),2(%a5,%d3.w)
        move.w  PKEY(%a4),4(%a5,%d3.w)
        move.w  DEPTH(%a4),6(%a5,%d3.w)
        move.w  8(%a5,%d2.w),8(%a5,%d3.w)
        move.w  %d3,8(%a5,%d2.w)
        move.w  %d0,0(%a5,%d2.w)
        move.w  %d0,2(%a6)
        move.w  %d0,GEND(%a4)
        add.w   #12,NN(%a4)
        rts
pgen:   lea     GAPS(%a4),%a6
ploop:  cmp.w   2(%a6),%d0              | gap ends at or before y0: next gap
        bge     pnext
        cmp.w   (%a6),%d1               | piece ends at or before this gap: done
        ble     pend
        move.w  NNLIM(%a4),%d3          | out of runs or gaps: drop (never seen)
        cmp.w   NN(%a4),%d3
        blt     pend
        cmp.w   #NGMAX,NG(%a4)
        bge     pend
        move.w  4(%a6),%d2              | n: the gap's run
        cmp.w   (%a6),%d0
        bgt     pbm
        cmp.w   2(%a6),%d1
        blt     ptop
        | the whole gap: n becomes the piece, the gap entry goes
        move.w  PID(%a4),2(%a5,%d2.w)
        move.w  PKEY(%a4),4(%a5,%d2.w)
        move.w  DEPTH(%a4),6(%a5,%d2.w)
        subq.w  #1,NG(%a4)
        move.l  %a6,%d3
3:      move.l  6(%a6),(%a6)
        move.w  10(%a6),4(%a6)
        addq.l  #6,%a6
        cmp.w   #32767,-6(%a6)
        bne.s   3b
        movea.l %d3,%a6
        bra     ploop
ptop:   | the top of the gap: n becomes the piece [a, y1), new uncovered run m = [y1, b)
        move.w  NN(%a4),%d3
        move.w  2(%a6),0(%a5,%d3.w)
        move.w  #-1,2(%a5,%d3.w)
        clr.w   4(%a5,%d3.w)
        move.w  #0x7FFF,6(%a5,%d3.w)
        move.w  8(%a5,%d2.w),8(%a5,%d3.w)
        move.w  %d1,0(%a5,%d2.w)
        move.w  PID(%a4),2(%a5,%d2.w)
        move.w  PKEY(%a4),4(%a5,%d2.w)
        move.w  DEPTH(%a4),6(%a5,%d2.w)
        move.w  %d3,8(%a5,%d2.w)
        move.w  %d1,(%a6)
        move.w  %d3,4(%a6)
        add.w   #12,NN(%a4)
        bra     pend
pbm:    cmp.w   2(%a6),%d1
        blt     pmid
        | the bottom of the gap: n stays uncovered [a, y0), new piece run m = [y0, b)
        move.w  NN(%a4),%d3
        move.w  2(%a6),0(%a5,%d3.w)
        move.w  PID(%a4),2(%a5,%d3.w)
        move.w  PKEY(%a4),4(%a5,%d3.w)
        move.w  DEPTH(%a4),6(%a5,%d3.w)
        move.w  8(%a5,%d2.w),8(%a5,%d3.w)
        move.w  %d3,8(%a5,%d2.w)
        move.w  %d0,0(%a5,%d2.w)
        move.w  %d0,2(%a6)
        add.w   #12,NN(%a4)
pnext:  addq.l  #6,%a6
        bra     ploop
pmid:   | inside the gap: n = [a, y0), m1 = piece [y0, y1), m2 = uncovered [y1, b)
        move.w  NN(%a4),%d3
        move.w  %d1,0(%a5,%d3.w)
        move.w  PID(%a4),2(%a5,%d3.w)
        move.w  PKEY(%a4),4(%a5,%d3.w)
        move.w  DEPTH(%a4),6(%a5,%d3.w)
        move.w  2(%a6),12(%a5,%d3.w)
        move.w  #-1,14(%a5,%d3.w)
        clr.w   16(%a5,%d3.w)
        move.w  #0x7FFF,18(%a5,%d3.w)
        move.w  8(%a5,%d2.w),20(%a5,%d3.w)
        move.w  %d3,8(%a5,%d2.w)
        move.w  %d0,0(%a5,%d2.w)
        add.w   #12,%d3
        move.w  %d3,-4(%a5,%d3.w)       | m1.next = m2
        | open a gap entry after this one: shift the rest (with the sentinel) up by 6
        move.l  %a6,%d2
        addq.l  #6,%d2                  | where the new entry goes
4:      addq.l  #6,%a6
        cmp.w   #32767,(%a6)
        bne.s   4b
5:      move.l  (%a6),6(%a6)
        move.w  4(%a6),10(%a6)
        cmpa.l  %d2,%a6
        beq.s   6f
        subq.l  #6,%a6
        bra.s   5b
6:      move.w  %d1,(%a6)               | new gap [y1, b) -> m2
        move.w  -4(%a6),2(%a6)
        move.w  %d3,4(%a6)
        move.w  %d0,-4(%a6)             | this gap now ends at y0
        addq.w  #1,NG(%a4)
        add.w   #24,NN(%a4)
pend:   move.w  GAPS(%a4),GSTART(%a4)   | first gap row (32767 when none)
        move.w  NG(%a4),%d2
        add.w   %d2,%d2
        move.w  %d2,%d3
        add.w   %d2,%d2
        add.w   %d3,%d2                 | 6 * NG
        lea     GAPS0(%a4),%a6
        adda.w  %d2,%a6
        move.l  %a6,LASTG(%a4)
        move.w  2(%a6),GEND(%a4)        | last gap end (-32768 when none)
pret:   rts

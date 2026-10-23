| Endurance M1: road renderer benchmark on the 1 MB Mac Plus.
| Renders a check frame (dumped and compared bit-exact with engine/road.py), then times
| NFR frames of: A full frame (geometry + draw + copy to screen), B geometry only,
| C draw only, D copy only, using the Ticks clock. Results go to the 'EDBG' block.
        .include "road_cfg.inc"
        .include "mac.inc"
        .equ    NFR, 120
        .equ    CHK_Z, 2600
        .equ    CHK_X, 30
| globals (below the 206 bytes of QuickDraw globals)
        .equ    g_buf, -212
        .equ    g_rows, -216
        .equ    g_scr, -220
        .equ    g_chk, -224
        .equ    g_vx, -226
        .equ    g_state, -300
        .equ    g_pats, -560
        .equ    g_dbg, -400
| debug block fields
        .equ    db_status, 8
        .equ    db_frames, 12
        .equ    db_chk, 16
        .equ    db_w, 20
        .equ    db_h, 22
        .equ    db_tA, 24
        .equ    db_tB, 28
        .equ    db_tC, 32
        .equ    db_tD, 36
        .equ    db_n, 40
        .equ    db_scr, 44
        .equ    db_bounds, 48
        .equ    db_buf, 56
        .equ    db_rbv, 60

| LEAF label, An: address of a label more than 32 KB away (68000 PC-relative is 16-bit only;
| GNU as silently wraps out-of-range displacements).
        .macro  LEAF label, reg
        lea     9f(%pc),\reg
        adda.l  #\label-9f,\reg
9:
        .endm

        .text
        .globl  _start
_start:
        pea     -4(%a5)
        _InitGraf
        _InitCursor
        _HideCursor
        lea     g_dbg(%a5),%a0
        move.l  #0x45444247,(%a0)       | 'EDBG'
        move.l  #~0x45444247,4(%a0)
        clr.l   db_status(%a0)
        clr.l   db_frames(%a0)
        move.w  #PF_W,db_w(%a0)
        move.w  #PF_H,db_h(%a0)
        move.l  #PF_RBV,db_rbv(%a0)
        move.l  #NFR,db_n(%a0)
        move.l  qd_top(%a5),db_bounds(%a0)
        move.l  qd_bottom(%a5),db_bounds+4(%a0)
| playfield position: centred in screenBits.bounds, left edge on a 16-pixel boundary
        move.w  qd_right(%a5),%d0
        sub.w   qd_left(%a5),%d0
        sub.w   #PF_W,%d0
        asr.w   #1,%d0
        add.w   qd_left(%a5),%d0
        and.w   #0xFFF0,%d0
        asr.w   #3,%d0
        ext.l   %d0
        move.w  qd_bottom(%a5),%d1
        sub.w   qd_top(%a5),%d1
        sub.w   #PF_H,%d1
        asr.w   #1,%d1
        add.w   qd_top(%a5),%d1
        mulu    qd_rowBytes(%a5),%d1
        add.l   %d0,%d1
        add.l   qd_baseAddr(%a5),%d1
        move.l  %d1,g_scr(%a5)
        move.l  %d1,db_scr(%a0)
| black screen
        movea.l qd_baseAddr(%a5),%a0
        move.w  qd_bottom(%a5),%d1
        sub.w   qd_top(%a5),%d1
        subq.w  #1,%d1
        move.w  qd_right(%a5),%d2
        sub.w   qd_left(%a5),%d2
        add.w   #15,%d2
        lsr.w   #4,%d2
        subq.w  #1,%d2
        moveq   #-1,%d0
1:      movea.l %a0,%a1
        move.w  %d2,%d3
2:      move.w  %d0,(%a1)+
        dbf     %d3,2b
        adda.w  qd_rowBytes(%a5),%a0
        dbf     %d1,1b
| buffers
        move.l  #PF_H*PF_RBV,%d0
        _NewPtrClear
        move.l  %a0,g_buf(%a5)
        beq     oom
        move.l  #8*(PF_KB+2),%d0
        _NewPtr
        move.l  %a0,g_rows(%a5)
        beq     oom
        move.l  #PF_H*PF_RBV,%d0
        _NewPtr
        move.l  %a0,g_chk(%a5)
        beq     oom
        lea     g_dbg(%a5),%a1
        move.l  %a0,db_chk(%a1)
        move.l  g_buf(%a5),db_buf(%a1)
| sky rows 0..HY into the buffer
        LEAF    m1_sky, %a0
        movea.l g_buf(%a5),%a1
        addq.l  #PF_MG*4,%a1
        move.w  #PF_HY,%d1
2:      move.w  #PF_NL-1,%d0
1:      move.l  (%a0)+,(%a1)+
        dbf     %d0,1b
        lea     PF_MG*8(%a1),%a1
        dbf     %d1,2b
| road state
        lea     g_state(%a5),%a0
        clr.l   rs_camz(%a0)
        clr.w   rs_camx(%a0)
        LEAF    m1_track, %a1
        move.l  %a1,rs_ring(%a0)
        move.l  %a1,rs_segp(%a0)
        move.l  (%a1),rs_segend(%a0)
        LEAF    m1_track_end, %a1
        move.l  %a1,rs_ringend(%a0)
        lea     g_pats(%a5),%a2
        move.l  %a2,rs_pats(%a0)
        LEAF    m1_pats, %a1
        LEAF    m1_pieces, %a3
        moveq   #3,%d0
1:      move.l  (%a1)+,(%a2)+
        move.l  (%a1)+,(%a2)+
        move.l  (%a1)+,(%a2)+
        move.l  (%a1)+,(%a2)+
        move.l  %a3,(%a2)+
        adda.w  #M1_LSZ,%a3
        move.l  %a3,(%a2)+
        adda.w  #M1_LSZ,%a3
        move.l  %a3,(%a2)+
        adda.w  #M1_MSZ,%a3
        clr.l   (%a2)+
        dbf     %d0,1b
        move.l  g_rows(%a5),rs_rows(%a0)
        move.w  #2,g_vx(%a5)
| check frame
        move.l  #CHK_Z,%d0
        bsr     cam_adv
        move.w  #CHK_X,g_state+rs_camx(%a5)
        bsr     frame
        movea.l g_buf(%a5),%a0
        movea.l g_chk(%a5),%a1
        move.w  #PF_H*PF_RBV/4-1,%d0
1:      move.l  (%a0)+,(%a1)+
        dbf     %d0,1b
        move.l  #1,g_dbg+db_status(%a5)
| A: full frames
        move.l  (Ticks).w,%d7
        move.w  #NFR-1,%d6
1:      bsr     step
        bsr     frame
        dbf     %d6,1b
        move.l  (Ticks).w,%d0
        sub.l   %d7,%d0
        move.l  %d0,g_dbg+db_tA(%a5)
| B: geometry only
        move.l  (Ticks).w,%d7
        move.w  #NFR-1,%d6
1:      bsr     step
        lea     g_state(%a5),%a0
        bsr     road_geom
        dbf     %d6,1b
        move.l  (Ticks).w,%d0
        sub.l   %d7,%d0
        move.l  %d0,g_dbg+db_tB(%a5)
| C: draw only
        move.l  (Ticks).w,%d7
        move.w  #NFR-1,%d6
1:      bsr     draw
        dbf     %d6,1b
        move.l  (Ticks).w,%d0
        sub.l   %d7,%d0
        move.l  %d0,g_dbg+db_tC(%a5)
| D: copy only
        move.l  (Ticks).w,%d7
        move.w  #NFR-1,%d6
1:      bsr     copy
        dbf     %d6,1b
        move.l  (Ticks).w,%d0
        sub.l   %d7,%d0
        move.l  %d0,g_dbg+db_tD(%a5)
        move.l  #2,g_dbg+db_status(%a5)
| keep animating
forever:
        bsr     step
        bsr     frame
        bra.s   forever

oom:    move.l  #0xDEAD,g_dbg+db_status(%a5)
1:      bra.s   1b

| step: advance the camera (vary the speed a little) and sweep camx between -40 and 40
step:   addq.l  #1,g_dbg+db_frames(%a5)
        moveq   #29,%d0
        move.l  g_dbg+db_frames(%a5),%d1
        and.w   #15,%d1
        add.w   %d1,%d0
        bsr     cam_adv
        move.w  g_vx(%a5),%d0
        add.w   %d0,g_state+rs_camx(%a5)
        move.w  g_state+rs_camx(%a5),%d1
        cmp.w   #40,%d1
        blt.s   1f
        move.w  #-2,g_vx(%a5)
1:      cmp.w   #-40,%d1
        bgt.s   2f
        move.w  #2,g_vx(%a5)
2:      rts

| cam_adv: camz += d0.l; keep segp/segend on the segment holding the bottom row
cam_adv:
        lea     g_state(%a5),%a0
        add.l   rs_camz(%a0),%d0
        move.l  %d0,rs_camz(%a0)
        add.l   #PF_ZNEAR,%d0
        movea.l rs_segp(%a0),%a1
1:      cmp.l   rs_segend(%a0),%d0
        bcs.s   3f
        addq.l  #8,%a1
        cmpa.l  rs_ringend(%a0),%a1
        bcs.s   2f
        movea.l rs_ring(%a0),%a1
2:      move.l  (%a1),%d1
        add.l   %d1,rs_segend(%a0)
        bra.s   1b
3:      move.l  %a1,rs_segp(%a0)
        rts

frame:  lea     g_state(%a5),%a0
        bsr     road_geom
        bsr     draw
        bra     copy

draw:   lea     g_state(%a5),%a0
        movea.l g_buf(%a5),%a1
        adda.w  #(PF_HY+1)*PF_RBV,%a1
        moveq   #0,%d0
        bra     road_draw

copy:   movea.l g_buf(%a5),%a0
        addq.l  #PF_MG*4,%a0
        movea.l g_scr(%a5),%a1
        move.w  #PF_H,%d0
        bra     pf_copy

        .include "road_kernel.s"
        .include "road_data.s"
        .include "m1_data.s"

| Narrow Screen Control Panel device (cdev).
|
| FUNCTION cdev(message, item, numItems, CPanelID: INTEGER; VAR theEvent: EventRecord;
|               cdevStorage: Handle; CPDialog: DialogPtr): LONGINT;
|
| Two horizontal scroll bars set the left margin (16-pixel steps) and the right
| margin (1-pixel steps). Dashed XOR guide lines on the physical screen show where
| the new edges will be. Settings are saved to the 'nsSt' resource in this file and
| take effect at the next restart (the INIT applies them at boot).

        .include "macdefs.inc"

| cdev messages
        .set initDev,     0
        .set hitDev,      1
        .set closeDev,    2
        .set activDev,    5
        .set deactivDev,  6

| Local dialog item numbers (added to numItems to get the real item number)
        .set iLeftBar,    4
        .set iLeftVal,    5
        .set iRightBar,   7
        .set iRightVal,   8
        .set iRestart,    10

| Control part codes
        .set inUpButton,  20
        .set inDownButton, 21
        .set inPageUp,    22
        .set inPageDown,  23

| Parameters (after LINK A6)
        .set pDialog,     8
        .set pStorage,    12
        .set pEvent,      16
        .set pNumItems,   22
        .set pItem,       24
        .set pMessage,    26
        .set pResult,     28
        .set kParamBytes, 20

| Locals
        .set lSavePort,   -4
        .set lItemH,      -8
        .set lKind,       -10
        .set lBox,        -18
        .set lPoint,      -22
        .set lStr,        -280
        .set kLocals,     -280

| Storage handle layout (locked while the cdev is open)
        .set sLeft,       0             | left margin, pixels
        .set sRight,      2             | right margin, pixels
        .set sGuideL,     4             | x of drawn left guide, or -1
        .set sGuideR,     6             | x of drawn right guide, or -1
        .set sNumItems,   8
        .set sLeftCtl,    10            | last known left scroll bar value
        .set sRightCtl,   12            | last known right scroll bar value
        .set sSaveL,      16            | pixels under the left guide (342 x 2)
        .set sSaveR,      16+684        | pixels under the right guide
        .set kStorage,    16+684+684


| Debug tracing (assemble with --defsym DEBUG=1): writes longs into the black left
| margin of the physical screen (rows 300+, bytes 0-3), which nothing else draws.
| Assumes the test build's 32-pixel left margin (ScrnBase shifted by 4 bytes).
        .macro TRACE row, src
        .ifdef DEBUG
        movem.l %d0/%a0,-(%sp)
        move.l  \src,%d0
        movea.l (ScrnBase).w,%a0
        move.l  %d0,(\row*64-4)(%a0)
        movem.l (%sp)+,%d0/%a0
        .endif
        .endm
        .macro TRACEINC row
        .ifdef DEBUG
        move.l  %a0,-(%sp)
        movea.l (ScrnBase).w,%a0
        addq.l  #1,(\row*64-4)(%a0)
        movea.l (%sp)+,%a0
        .endif
        .endm

        .text
        .globl _start
_start:
        TRACEINC 300
        link    %a6,#kLocals
        movem.l %d3-%d7/%a2-%a4,-(%sp)
        move.l  pStorage(%a6),pResult(%a6)

        pea     lSavePort(%a6)
        _GetPort
        move.l  pDialog(%a6),-(%sp)
        _SetPort

        TRACE   301, pItem(%a6)             | item<<16 | message
        move.w  pMessage(%a6),%d0
        beq.w   msg_init

        move.l  pStorage(%a6),%d1
        cmpi.l  #0x1000,%d1             | not a real handle (e.g. cdevUnset): ignore
        bcs.w   done
        movea.l %d1,%a0
        movea.l (%a0),%a4               | a4 = storage (locked)

        cmpi.w  #hitDev,%d0
        beq.w   msg_hit
        cmpi.w  #closeDev,%d0
        beq.w   msg_close
        cmpi.w  #activDev,%d0
        beq.w   msg_activate
        cmpi.w  #deactivDev,%d0
        beq.w   msg_deactivate
        bra.w   done

msg_init:
        move.l  #kStorage,%d0
        _NewHandle
        move.l  %a0,%d0
        bne.s   1f
        clr.l   pResult(%a6)            | cdevMemErr
        bra.w   done
1:      movea.l %a0,%a3
        _HLock
        movea.l (%a3),%a4
        move.l  %a3,pResult(%a6)
        move.w  pNumItems(%a6),sNumItems(%a4)
        move.l  #-1,sGuideL(%a4)        | no guides drawn (both words -1)
        bsr.w   load_settings
        move.w  sLeftCtl(%a4),%d3
        moveq   #iLeftBar,%d1
        bsr.w   set_ctl
        move.w  sRightCtl(%a4),%d3
        moveq   #iRightBar,%d1
        bsr.w   set_ctl
        bsr.w   update_texts
        bsr.w   erase_guides
        bsr.w   draw_guides
        bra.w   done

msg_activate:
        bsr.w   erase_guides
        bsr.w   draw_guides
        bra.w   done

msg_deactivate:
        bsr.w   erase_guides
        bra.w   done

msg_close:
        bsr.w   erase_guides
        movea.l pStorage(%a6),%a0
        _DisposHandle
        bra.w   done

msg_hit:
        TRACE   302, #1
        move.w  pItem(%a6),%d0
        sub.w   sNumItems(%a4),%d0
        cmpi.w  #iLeftBar,%d0
        beq.w   hit_left
        cmpi.w  #iRightBar,%d0
        beq.w   hit_right
        cmpi.w  #iRestart,%d0
        beq.w   hit_restart
        bra.w   done

hit_left:
        moveq   #iLeftBar,%d1
        bsr.w   get_item
        moveq   #1,%d4                  | arrow step (x16 px)
        moveq   #2,%d5                  | page step
        moveq   #kMaxLeft/16,%d6        | max
        move.w  sLeftCtl(%a4),%d7
        TRACE   302, #2
        bsr.w   track_value
        TRACE   302, #3
        move.w  %d0,sLeftCtl(%a4)
        lsl.w   #4,%d0
        move.w  %d0,sLeft(%a4)
        bra.w   changed

hit_right:
        moveq   #iRightBar,%d1
        bsr.w   get_item
        moveq   #1,%d4                  | arrow step (px)
        moveq   #8,%d5                  | page step
        move.w  #kMaxRight,%d6
        move.w  sRightCtl(%a4),%d7
        bsr.w   track_value
        move.w  %d0,sRightCtl(%a4)
        move.w  %d0,sRight(%a4)

changed:
        TRACE   302, #4
        bsr.w   erase_guides
        TRACE   302, #5
        bsr.w   update_texts
        TRACE   302, #6
        bsr.w   draw_guides
        TRACE   302, #7
        bsr.w   save_settings
        TRACE   302, #8
        bra.s   done

hit_restart:
        bsr.w   erase_guides
        bsr.w   save_settings
        move.w  #2,-(%sp)               | ShutDwnStart
        _ShutDown

done:
        move.l  lSavePort(%a6),-(%sp)
        _SetPort
        movem.l (%sp)+,%d3-%d7/%a2-%a4
        unlk    %a6
        movea.l (%sp)+,%a0
        lea     kParamBytes(%sp),%sp
        jmp     (%a0)

| track_value: a2 = scroll bar, d4 = arrow step, d5 = page step, d6 = max,
| d7 = value before the click. Returns the new value in d0 and sets the control.
| The Control Panel has already tracked the click: a dragged thumb has changed
| the value; arrow and page clicks have not, so we apply them ourselves.
track_value:
        subq.l  #2,%sp
        move.l  %a2,-(%sp)
        _GetCtlValue
        move.w  (%sp)+,%d3
        cmp.w   %d7,%d3
        bne.s   tv_set                  | thumb was dragged

        movea.l pEvent(%a6),%a0
        move.l  10(%a0),lPoint(%a6)     | theEvent.where
        pea     lPoint(%a6)
        _GlobalToLocal
        subq.l  #2,%sp
        move.l  lPoint(%a6),-(%sp)
        move.l  pDialog(%a6),-(%sp)
        pea     lItemH(%a6)
        _FindControl
        move.w  (%sp)+,%d0
        TRACE   303, %d0                   | part code
        cmpi.w  #inUpButton,%d0
        bne.s   1f
        sub.w   %d4,%d3
        bra.s   tv_clamp
1:      cmpi.w  #inDownButton,%d0
        bne.s   2f
        add.w   %d4,%d3
        bra.s   tv_clamp
2:      cmpi.w  #inPageUp,%d0
        bne.s   3f
        sub.w   %d5,%d3
        bra.s   tv_clamp
3:      cmpi.w  #inPageDown,%d0
        bne.s   tv_clamp
        add.w   %d5,%d3
tv_clamp:
        tst.w   %d3
        bge.s   4f
        moveq   #0,%d3
4:      cmp.w   %d6,%d3
        ble.s   tv_set
        move.w  %d6,%d3
tv_set:
        move.l  %a2,-(%sp)
        move.w  %d3,-(%sp)
        _SetCtlValue
        move.w  %d3,%d0
        rts

| get_item: d1 = local item number. Returns a2 = item handle (box in lBox).
get_item:
        move.l  pDialog(%a6),-(%sp)
        move.w  sNumItems(%a4),%d0
        add.w   %d1,%d0
        move.w  %d0,-(%sp)
        pea     lKind(%a6)
        pea     lItemH(%a6)
        pea     lBox(%a6)
        _GetDItem
        movea.l lItemH(%a6),%a2
        rts

| set_ctl: d1 = local item, d3 = value.
set_ctl:
        bsr.s   get_item
        move.l  %a2,-(%sp)
        move.w  %d3,-(%sp)
        _SetCtlValue
        rts

| update_texts: show the margins in pixels next to the scroll bars.
update_texts:
        moveq   #iLeftVal,%d1
        move.w  sLeft(%a4),%d3
        bsr.s   set_num
        moveq   #iRightVal,%d1
        move.w  sRight(%a4),%d3
| set_num: d1 = local item, d3 = number.
set_num:
        bsr.s   get_item
        moveq   #0,%d0
        move.w  %d3,%d0
        lea     lStr(%a6),%a0
        move.w  #0,-(%sp)               | NumToString selector
        _Pack7
        move.l  %a2,-(%sp)
        pea     lStr(%a6)
        _SetIText
        pea     lBox(%a6)               | SetIText doesn't redraw static text
        _EraseRect                      | on this ROM, so draw it ourselves
        pea     (lStr+1)(%a6)
        moveq   #0,%d0
        move.b  lStr(%a6),%d0
        move.l  %d0,-(%sp)
        pea     lBox(%a6)
        clr.w   -(%sp)                  | teJustLeft
        _TextBox
        rts

| Guides: a 3-pixel vertical line (white, black, white) at each new edge, drawn
| on the physical screen so it shows on grey desktop, black margin or white
| window alike. The two bytes under each row are saved first and put back when
| the guide is erased.
draw_guides:
        move.w  sLeft(%a4),%d0
        move.w  %d0,sGuideL(%a4)
        lea     sSaveL(%a4),%a1
        bsr.s   draw_guide
        move.w  #511,%d0
        sub.w   sRight(%a4),%d0
        move.w  %d0,sGuideR(%a4)
        lea     sSaveR(%a4),%a1
        bra.s   draw_guide

erase_guides:
        move.w  sGuideL(%a4),%d0
        bmi.s   1f
        lea     sSaveL(%a4),%a1
        bsr.s   restore_guide
        move.w  #-1,sGuideL(%a4)
1:      move.w  sGuideR(%a4),%d0
        bmi.s   2f
        lea     sSaveR(%a4),%a1
        bsr.s   restore_guide
        move.w  #-1,sGuideR(%a4)
2:      rts

| draw_guide: d0 = guide x (0..511), a1 = save area (342 rows x 2 bytes).
draw_guide:
        movem.l %d3-%d6/%a2,-(%sp)
        move.w  %d0,%d4
        movea.l %a1,%a2
        _HideCursor
        move.w  %d4,%d0
        bsr.s   guide_addr              | a0 = 2-byte span on row 0, d3 = its x
        move.w  %d4,%d5
        sub.w   %d3,%d5                 | d5 = guide x within the span
        move.w  #341,%d6
1:      move.b  (%a0),(%a2)+
        move.b  1(%a0),(%a2)+
        move.w  %d5,%d0
        subq.w  #1,%d0
        bsr.s   span_clr                | x-1 white
        move.w  %d5,%d0
        bsr.s   span_set                | x   black
        move.w  %d5,%d0
        addq.w  #1,%d0
        bsr.s   span_clr                | x+1 white
        lea     64(%a0),%a0
        dbf     %d6,1b
        _ShowCursor
        movem.l (%sp)+,%d3-%d6/%a2
        rts

| restore_guide: d0 = guide x, a1 = save area filled by draw_guide.
restore_guide:
        movem.l %d3-%d6/%a2,-(%sp)
        move.w  %d0,%d4
        movea.l %a1,%a2
        _HideCursor
        move.w  %d4,%d0
        bsr.s   guide_addr
        move.w  #341,%d6
1:      move.b  (%a2)+,(%a0)
        move.b  (%a2)+,1(%a0)
        lea     64(%a0),%a0
        dbf     %d6,1b
        _ShowCursor
        movem.l (%sp)+,%d3-%d6/%a2
        rts

| guide_addr: d0 = guide x. Returns a0 = address on row 0 of the 2-byte span
| holding pixels x-1..x+1 (clamped to the row), d3 = x of the span's first pixel.
guide_addr:
        move.w  %d0,%d3
        subq.w  #1,%d3
        bpl.s   1f
        moveq   #0,%d3
1:      lsr.w   #3,%d3
        cmpi.w  #62,%d3
        bls.s   2f
        moveq   #62,%d3
2:      move.l  (ScrnBase).w,%d0
        andi.l  #0xFFFFFF80,%d0         | physical base (INIT shifts by < 128 bytes)
        movea.l %d0,%a0
        adda.w  %d3,%a0
        lsl.w   #3,%d3
        rts

| span_set / span_clr: d0 = pixel offset within the span at a0; offsets outside
| 0..15 are ignored. Clobbers d0-d1.
span_set:
        bsr.s   span_bit
        bmi.s   1f
        bset    %d0,0(%a0,%d1.w)
1:      rts
span_clr:
        bsr.s   span_bit
        bmi.s   1f
        bclr    %d0,0(%a0,%d1.w)
1:      rts
span_bit:
        cmpi.w  #15,%d0
        bhi.s   1f                      | negative or past the span
        move.w  %d0,%d1
        lsr.w   #3,%d1                  | d1 = byte within span
        andi.w  #7,%d0
        neg.w   %d0
        addq.w  #7,%d0                  | d0 = 68000 bit number (N clear)
        rts
1:      moveq   #-1,%d0                 | N set: outside
        rts

| load_settings: read 'nsSt' into storage (defaults 0), validate.
load_settings:
        clr.l   sLeft(%a4)
        subq.l  #4,%sp
        move.l  #kSettingsType,-(%sp)
        move.w  #kResID,-(%sp)
        _GetResource
        movea.l (%sp)+,%a0
        move.l  %a0,%d0
        beq.s   3f
        movea.l (%a0),%a0
        move.w  stLeft(%a0),%d0
        andi.w  #0xFFF0,%d0
        cmpi.w  #kMaxLeft,%d0
        bls.s   1f
        moveq   #0,%d0
1:      move.w  %d0,sLeft(%a4)
        move.w  stRight(%a0),%d0
        cmpi.w  #kMaxRight,%d0
        bls.s   2f
        moveq   #0,%d0
2:      move.w  %d0,sRight(%a4)
3:      move.w  sLeft(%a4),%d0
        lsr.w   #4,%d0
        move.w  %d0,sLeftCtl(%a4)
        move.w  sRight(%a4),sRightCtl(%a4)
        rts

| save_settings: write storage values back into 'nsSt' in this file.
save_settings:
        subq.l  #4,%sp
        move.l  #kSettingsType,-(%sp)
        move.w  #kResID,-(%sp)
        _GetResource
        movea.l (%sp)+,%a2
        move.l  %a2,%d0
        beq.s   1f
        movea.l (%a2),%a0
        move.w  #1,stVersion(%a0)
        move.w  sLeft(%a4),stLeft(%a0)
        move.w  sRight(%a4),stRight(%a0)
        move.l  %a2,-(%sp)
        _ChangedResource
        move.l  %a2,-(%sp)
        _WriteResource
        subq.l  #2,%sp
        move.l  %a2,-(%sp)
        _HomeResFile
        _UpdateResFile                  | refNum left on stack by HomeResFile
1:      rts

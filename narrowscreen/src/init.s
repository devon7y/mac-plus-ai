| Narrow Screen INIT: makes System 6 treat the Mac Plus screen as a narrower,
| inset rectangle so content stays out of the columns hidden behind the bezel.
|
| How: QuickDraw's InitGraf fills screenBits from trap ScrnBitMap (0xA833), which
| on the Plus ROM returns baseAddr = ScrnBase, rowBytes = ScreenRow and a
| hard-coded 512x342 bounds. We shift ScrnBase right by the left margin (so the
| cursor and every app's origin move with it), replace ScrnBitMap with a version
| that reports the narrower width, pin the cursor, and paint the margins black.
| Everything after this INIT (Finder, apps) picks up the new screen at InitGraf.
|
| Fail-safe: does nothing unless this is a Mac Plus ROM with a 64-byte-row
| screen whose base is unshifted, settings are valid and non-zero, and Shift is
| not held down.

        .include "macdefs.inc"

        .text
        .globl _start
_start:
        movem.l %d0-%d7/%a0-%a6,-(%sp)

        movea.l (ROMBase).w,%a0
        cmpi.w  #PlusROMVers,8(%a0)
        bne.w   exit
        cmpi.w  #64,(ScreenRow).w
        bne.w   exit
        move.l  (ScrnBase).w,%d0
        andi.l  #0x7F,%d0               | Plus screen buffers are 128-byte aligned;
        bne.w   exit                    | non-zero means someone already shifted it
        btst    #0,(KeyMapShift).w      | Shift held: skip
        bne.w   exit

        subq.l  #4,%sp
        move.l  #kSettingsType,-(%sp)
        move.w  #kResID,-(%sp)
        _Get1Resource
        movea.l (%sp)+,%a0
        move.l  %a0,%d0
        beq.w   exit
        movea.l (%a0),%a0
        move.w  stLeft(%a0),%d6         | d6 = left margin (pixels)
        move.w  stRight(%a0),%d7        | d7 = right margin (pixels)
        andi.w  #0xFFF0,%d6             | left must keep ScrnBase word-aligned
        cmpi.w  #kMaxLeft,%d6
        bhi.w   exit
        cmpi.w  #kMaxRight,%d7
        bhi.w   exit
        move.w  %d6,%d0
        or.w    %d7,%d0
        beq.w   exit                    | both zero: nothing to do

        move.w  #512,%d5
        sub.w   %d6,%d5
        sub.w   %d7,%d5                 | d5 = usable width

        move.l  #(patch_end-patch_start),%d0
        _NewPtrSys
        move.l  %a0,%d0
        beq.w   exit
        movea.l %a0,%a3                 | a3 = resident patch in system heap
        lea     patch_start(%pc),%a0
        movea.l %a3,%a1
        move.l  #(patch_end-patch_start),%d0
        _BlockMove
        move.w  %d5,(patch_width-patch_start)(%a3)

        _HideCursor

| Paint the margins black. Build a 64-byte mask for one row on the stack,
| then OR it into all 342 rows of the physical screen.
        movea.l (ScrnBase).w,%a2        | a2 = physical screen base
        lea     -64(%sp),%sp
        movea.l %sp,%a1
        moveq   #0,%d1                  | d1 = x of this byte's first pixel
        move.w  #512,%d2
        sub.w   %d7,%d2                 | d2 = first pixel of right margin
        moveq   #63,%d3
mkmask: moveq   #0,%d0
        cmp.w   %d6,%d1
        bcs.s   mfull                   | x < left margin
        cmp.w   %d2,%d1
        bcc.s   mfull                   | x >= right margin start
        move.w  %d1,%d4
        addq.w  #8,%d4
        cmp.w   %d2,%d4
        bls.s   mstore                  | whole byte is usable
        move.w  %d2,%d4
        sub.w   %d1,%d4                 | right margin starts mid-byte
        move.w  #0xFF,%d0
        lsr.w   %d4,%d0
        bra.s   mstore
mfull:  move.w  #0xFF,%d0
mstore: move.b  %d0,(%a1)+
        addq.w  #8,%d1
        dbf     %d3,mkmask

        movea.l %a2,%a4
        move.w  #341,%d3
rows:   movea.l %sp,%a1
        moveq   #63,%d4
cols:   move.b  (%a1)+,%d0
        or.b    %d0,(%a4)+
        dbf     %d4,cols
        dbf     %d3,rows
        lea     64(%sp),%sp

        moveq   #0,%d0
        move.w  %d6,%d0
        lsr.w   #3,%d0
        add.l   %d0,(ScrnBase).w        | shift origin right by left margin

        move.w  %d5,(CrsrPinR).w        | pin cursor inside new width
        move.w  (Mouse+2).w,%d0
        cmp.w   %d5,%d0
        blt.s   1f
        move.w  %d5,%d0
        subq.w  #1,%d0
        move.w  %d0,(Mouse+2).w
        move.w  %d0,(MTemp+2).w
        move.w  %d0,(RawMouse+2).w
1:
        movea.l %a3,%a0
        move.w  #ScrnBitMapNum,%d0
        _SetToolTrapAddress

        _ShowCursor
exit:
        movem.l (%sp)+,%d0-%d7/%a0-%a6
        rts

| Replacement for ScrnBitMap (PROCEDURE ScrnBitMap(VAR bm: BitMap)).
| Copied into the system heap, so it must be position-independent.
        .even
patch_start:
        movea.l (%sp)+,%a1              | return address
        movea.l (%sp)+,%a0              | ^BitMap
        move.l  (ScrnBase).w,(%a0)+     | baseAddr (already shifted)
        move.w  (ScreenRow).w,(%a0)+    | rowBytes
        clr.l   (%a0)+                  | bounds.top, bounds.left
        move.w  #342,(%a0)+             | bounds.bottom
        move.w  patch_width(%pc),(%a0)  | bounds.right
        jmp     (%a1)
patch_width:
        .short  512
patch_end:

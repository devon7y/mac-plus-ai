| VBL task for Blockworld's sound effects (the PvZ port's design, pvz/port/mac/sound.s).
| The Plus (and SE) plays 370 samples per video frame from its sound buffer (high byte of each of
| 370 words, SoundBase = $266) at 22257 Hz. Each VBL this copies the next 370 bytes of the playing
| effect into those high bytes with MOVEP (the low bytes, the disk-speed buffer, are left alone),
| or writes silence once when nothing plays.
|
| Called by the Vertical Retrace Manager with A0 = the task record (SndTask in sound.c):
|   0 qLink, 4 qType, 6 vblAddr, 10 vblCount, 12 vblPhase, 14 pad,
|   16 src (next sample), 20 frames (left to play), 24 buf (sound buffer), 28 silent (flag)
| May use D0-D3/A0-A3 only.
        .text
        .globl  snd_vbl
snd_vbl:
        move.w  #1,10(%a0)              | run again next frame
        move.l  24(%a0),%a2
        move.l  20(%a0),%d0
        beq.s   .Squiet
        subq.l  #1,%d0
        move.l  %d0,20(%a0)
        move.l  16(%a0),%a1
        moveq   #45,%d1                 | 46 x 8 samples = 368
.Scopy: move.l  (%a1)+,%d0
        movep.l %d0,0(%a2)
        move.l  (%a1)+,%d0
        movep.l %d0,8(%a2)
        lea     16(%a2),%a2
        dbra    %d1,.Scopy
        move.w  (%a1)+,%d0              | the last 2
        movep.w %d0,0(%a2)
        move.l  %a1,16(%a0)
        clr.l   28(%a0)
        rts
.Squiet:
        tst.l   28(%a0)
        bne.s   .Sdone
        move.l  #0x80808080,%d0
        moveq   #45,%d1
.Sfill: movep.l %d0,0(%a2)
        movep.l %d0,8(%a2)
        lea     16(%a2),%a2
        dbra    %d1,.Sfill
        movep.w %d0,0(%a2)
        move.l  #1,28(%a0)
.Sdone: rts

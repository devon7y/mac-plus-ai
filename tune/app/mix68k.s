| mix68k.s: Tune's sound mixer and interrupt glue (68000, GNU as, Motorola syntax).
|
| Two voices at half the Plus's sound rate (11.1 kHz; each sample is written to two consecutive
| sound-buffer words): a melody voice reading a 256-byte wavetable (already scaled to the note's
| volume and biased by $80) with an 8.16 phase, and a bass voice reading a 64 KB table (a 256-sample
| wave with every sample repeated 256 times, so the 16-bit phase indexes it directly).
|
| Retro68 code reaches globals by absolute (relocated) addresses, so the interrupt glue needs
| neither A5 nor the task record; all registers are saved.

        .text
        .globl  snd_vbl_glue
snd_vbl_glue:
        movem.l %d0-%d7/%a0-%a6,-(%sp)
        jsr     snd_vbl_tick
        movem.l (%sp)+,%d0-%d7/%a0-%a6
        rts

        .globl  snd_tm_glue
snd_tm_glue:
        movem.l %d0-%d7/%a0-%a6,-(%sp)
        jsr     snd_tm_tick
        movem.l (%sp)+,%d0-%d7/%a0-%a6
        rts

| void mix_pairs(unsigned char *dst, long npairs, MixState *s)
|   MixState: 0 mtab (long), 4 mfrac, 6 midx, 8 mstepf, 10 mstepi, 12 btab (middle, long),
|             16 bphase, 18 bstep   (words)
|   dst: the sound-buffer word of the first sample (its high byte is the sample)
        .globl  mix_pairs
mix_pairs:
        movem.l %d2-%d7/%a2,-(%sp)
        movea.l 32(%sp),%a2             | dst
        move.l  36(%sp),%d7             | npairs
        movea.l 40(%sp),%a1             | s
        movea.l (%a1),%a0               | melody table
        moveq   #0,%d0
        moveq   #0,%d1
        moveq   #0,%d3
        move.w  4(%a1),%d0              | frac
        move.w  6(%a1),%d1
        andi.w  #0xFF,%d1               | index (byte)
        move.w  8(%a1),%d2              | step frac
        move.w  10(%a1),%d3             | step int (byte)
        move.w  16(%a1),%d4             | bass phase
        move.w  18(%a1),%d6             | bass step
        move.l  %a1,-(%sp)
        movea.l 12(%a1),%a1             | bass table middle
        move.w  %d7,-(%sp)              | remainder (npairs & 3) for the second loop
        andi.w  #3,(%sp)
        lsr.w   #2,%d7                  | groups of 4 pairs
        subq.w  #1,%d7
        bmi.s   2f
1:
        add.w   %d2,%d0
        addx.b  %d3,%d1
        move.b  0(%a0,%d1.w),%d5
        add.w   %d6,%d4
        add.b   0(%a1,%d4.w),%d5
        move.b  %d5,0(%a2)
        move.b  %d5,2(%a2)
        add.w   %d2,%d0
        addx.b  %d3,%d1
        move.b  0(%a0,%d1.w),%d5
        add.w   %d6,%d4
        add.b   0(%a1,%d4.w),%d5
        move.b  %d5,4(%a2)
        move.b  %d5,6(%a2)
        add.w   %d2,%d0
        addx.b  %d3,%d1
        move.b  0(%a0,%d1.w),%d5
        add.w   %d6,%d4
        add.b   0(%a1,%d4.w),%d5
        move.b  %d5,8(%a2)
        move.b  %d5,10(%a2)
        add.w   %d2,%d0
        addx.b  %d3,%d1
        move.b  0(%a0,%d1.w),%d5
        add.w   %d6,%d4
        add.b   0(%a1,%d4.w),%d5
        move.b  %d5,12(%a2)
        move.b  %d5,14(%a2)
        lea     16(%a2),%a2
        dbf     %d7,1b
2:      move.w  (%sp)+,%d7
        subq.w  #1,%d7
        bmi.s   9f
3:
        add.w   %d2,%d0
        addx.b  %d3,%d1
        move.b  0(%a0,%d1.w),%d5
        add.w   %d6,%d4
        add.b   0(%a1,%d4.w),%d5
        move.b  %d5,0(%a2)
        move.b  %d5,2(%a2)
        addq.l  #4,%a2
        dbf     %d7,3b
9:      movea.l (%sp)+,%a1
        move.w  %d0,4(%a1)
        move.w  %d1,6(%a1)
        move.w  %d4,16(%a1)
        movem.l (%sp)+,%d2-%d7/%a2
        rts

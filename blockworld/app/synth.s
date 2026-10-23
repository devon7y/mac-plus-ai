| synth.s: a sound effect part for the 68000 (the same samples as synth_part_c in synth.c).
| Frequency and amplitude step every 32 samples; per sample the square wave costs about 45 cycles.
|
| void synth_part_asm(unsigned char *out, long len, long f, long df, long a, long da, long kind,
|                     unsigned short *lfsr)

        .text
        .globl  synth_part_asm
        .equ    RATE, 22257
        .equ    A_OUT, 36               | (the arguments, after the saved registers)
        .equ    A_LEN, 40
        .equ    A_F, 44
        .equ    A_DF, 48                | (32 df once scaled)
        .equ    A_A, 52
        .equ    A_DA, 56                | (32 da)
        .equ    A_KIND, 62              | (the low word)
        .equ    A_LFSR, 64

| registers: a0 out, a2 lfsr, d1 f, d3 a, d7 samples left, d6 this chunk's count (for dbra),
| d4 phase / hold, d0 twice the frequency (square) / the frequency (noise), d5 the value written,
| d2 high (square: a1 holds the other value) / the amplitude (noise)
synth_part_asm:
        movem.l %d2-%d7/%a2-%a3,-(%sp)
        movea.l A_OUT(%sp),%a0
        move.l  A_LEN(%sp),%d7
        move.l  A_F(%sp),%d1
        move.l  A_A(%sp),%d3
        movea.l A_LFSR(%sp),%a2
        move.l  A_DF(%sp),%d0
        lsl.l   #5,%d0
        move.l  %d0,A_DF(%sp)
        move.l  A_DA(%sp),%d0
        lsl.l   #5,%d0
        move.l  %d0,A_DA(%sp)
        moveq   #0,%d4
        moveq   #-1,%d2                 | (the square wave starts high)
        moveq   #-128,%d5               | (the noise starts at 128)
chunk:  tst.l   %d7
        ble     99f
        moveq   #32,%d6                 | this chunk: 32 samples or the rest
        cmp.l   %d6,%d7
        bhs.s   1f
        move.w  %d7,%d6
1:      sub.l   %d6,%d7
        subq.w  #1,%d6
        move.l  %d1,%d0
        swap    %d0                     | the frequency now
        tst.w   %d0
        ble.s   quiet
        cmpi.w  #1,A_KIND(%sp)
        beq.s   square
        cmpi.w  #2,A_KIND(%sp)
        beq.s   noise
quiet:  move.b  #-128,(%a0)+            | 128s (no phase change)
        dbra    %d6,quiet
        bra     next

square: add.w   %d0,%d0                 | the phase step: twice the frequency
        move.l  %d3,%d5
        swap    %d5                     | the amplitude now
        neg.b   %d5
        add.b   #-128,%d5               | low: 128 - a
        movea.l %d5,%a1
        move.l  %d3,%d5
        swap    %d5
        add.b   #-128,%d5               | high: 128 + a
        tst.b   %d2
        bne.s   2f
        exg     %d5,%a1                 | (low now: the other one is high)
2:      add.w   %d0,%d4
        cmp.w   #RATE,%d4
        blo.s   3f
        sub.w   #RATE,%d4               | a flip
        not.b   %d2
        exg     %d5,%a1
3:      move.b  %d5,(%a0)+
        dbra    %d6,2b
        bra.s   next

noise:  move.l  %d3,%d2
        swap    %d2                     | the amplitude now
4:      add.w   %d0,%d4
        cmp.w   #RATE,%d4
        blo.s   6f
        sub.w   #RATE,%d4               | a new value
        lsr.w   (%a2)                   | (the LFSR's next step)
        bcc.s   5f
        eori.w  #0xB400,(%a2)
5:      moveq   #0,%d5
        move.b  1(%a2),%d5
        sub.w   #128,%d5
        muls.w  %d2,%d5
        asr.l   #7,%d5
        add.b   #-128,%d5
6:      move.b  %d5,(%a0)+
        dbra    %d6,4b

next:   add.l   A_DF(%sp),%d1
        add.l   A_DA(%sp),%d3
        bra     chunk
99:     movem.l (%sp)+,%d2-%d7/%a2-%a3
        rts

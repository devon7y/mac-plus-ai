| lz.s: the save file's LZ codec for the 68000 (the same as lz_unpack_c and lz_pack_c in lz.c).
|
| short lz_unpack_asm(const unsigned char *in, long len, unsigned char *out, long n)
|   Unpacks exactly n bytes. 0, or -1 if the data is bad. Short literals (up to 14) and matches
|   (up to 17) are copied by unrolled moves without checks, so bad data may write up to 17 bytes
|   past out + n and read up to 17 past in + len (LZ_SLACK in lz.h); longer ones are checked.
|   Match offsets stay inside what is already written.

        .text
        .globl  lz_unpack_asm

| registers: a0 in, a1 out, d4 out's end, d6 in's end, d7 out's start
lz_unpack_asm:
        movem.l %d2-%d7/%a2,-(%sp)
        movea.l 32(%sp),%a0
        move.l  36(%sp),%d6
        add.l   %a0,%d6
        movea.l 40(%sp),%a1
        move.l  %a1,%d7
        move.l  44(%sp),%d4
        add.l   %a1,%d4
token:  cmpa.l  %d4,%a1
        bhs     ok
        cmpa.l  %d6,%a0
        bhs     bad
        moveq   #0,%d0
        move.b  (%a0)+,%d0              | the token
        move.w  %d0,%d1
        lsr.w   #4,%d1                  | the literals
        beq.s   match
        cmp.w   #15,%d1
        beq     longlit
        add.w   %d1,%d1
        neg.w   %d1
        jmp     1f(%pc,%d1.w)
        .rept   14
        move.b  (%a0)+,(%a1)+
        .endr
1:      cmpa.l  %d4,%a1
        bhs     ok
match:  moveq   #0,%d2
        move.b  (%a0)+,%d2
        lsl.w   #8,%d2
        move.b  (%a0)+,%d2              | the offset (1..65535)
        beq.s   2f
3:      movea.l %a1,%a2
        suba.l  %d2,%a2                 | the match's start, inside what is written
        cmpa.l  %d7,%a2
        blo     bad
        and.w   #15,%d0
        cmp.w   #15,%d0
        beq.s   longmatch
        add.w   %d0,%d0                 | 3 + d0 bytes
        neg.w   %d0
        jmp     4f(%pc,%d0.w)
        .rept   14
        move.b  (%a2)+,(%a1)+
        .endr
4:      move.b  (%a2)+,(%a1)+
        move.b  (%a2)+,(%a1)+
        move.b  (%a2)+,(%a1)+
        bra     token
2:      tst.w   %d2                     | (an offset of 256 * k has a zero low byte)
        bne.s   3b
        bra     bad
longmatch:
        moveq   #15,%d2
        bsr     more
        bmi     bad
        addq.l  #3,%d2
        move.l  %d4,%d1                 | no more than the room left
        sub.l   %a1,%d1
        cmp.l   %d1,%d2
        bhi     bad
        bsr     copy
        bra     token
longlit:
        moveq   #15,%d2
        bsr     more
        bmi     bad
        move.l  %d4,%d1                 | no more than the room left and the input left
        sub.l   %a1,%d1
        cmp.l   %d1,%d2
        bhi     bad
        move.l  %d6,%d1
        sub.l   %a0,%d1
        cmp.l   %d1,%d2
        bhi     bad
        movea.l %a0,%a2
        adda.l  %d2,%a0
        bsr     copy
        cmpa.l  %d4,%a1
        bhs.s   ok
        bra     match
ok:     cmpa.l  %d4,%a1                 | (exactly to the end)
        bne.s   bad
        cmpa.l  %d6,%a0
        bhi.s   bad
        moveq   #0,%d0
        bra.s   9f
bad:    moveq   #-1,%d0
9:      movem.l (%sp)+,%d2-%d7/%a2
        rts

| d2 += the length bytes that follow (255: another one follows). N set if the input ran out.
more:   cmpa.l  %d6,%a0
        bhs.s   2f
        moveq   #0,%d1
        move.b  (%a0)+,%d1
        add.l   %d1,%d2
        cmp.w   #255,%d1
        beq.s   more
        moveq   #0,%d1
        rts
2:      moveq   #-1,%d1
        rts

| copy d2 (>= 1) bytes from (a2)+ to (a1)+ (byte by byte, so overlapping matches repeat)
copy:   moveq   #15,%d1
        and.w   %d2,%d1
        lsr.l   #4,%d2                  | whole blocks of 16
        add.w   %d1,%d1
        neg.w   %d1
        jmp     3f(%pc,%d1.w)           | the rest first
4:
        .rept   16
        move.b  (%a2)+,(%a1)+
        .endr
3:      dbra    %d2,4b
        sub.l   #0x10000,%d2            | (more than 65535 blocks)
        bpl.s   4b
        rts

| long lz_pack_asm(const unsigned char *in, long n, unsigned char *out, void *work)
|   The same bytes as lz_pack_c in lz.c: greedy; the one candidate is the last position with the
|   same hash of 3 bytes ((p0 << 6 ^ p1 << 3 ^ p2) & 4095; work: 4096 pointers); positions inside
|   a match are not remembered. Returns the packed length.
| registers: a0 q, a2 out, a3 the hash table, a4 the literals' start, a6 the match's end,
| d1 the offset, d3 the match length, d6 the last position with 3 bytes, d7 the end
        .globl  lz_pack_asm
lz_pack_asm:
        movem.l %d2-%d7/%a2-%a4/%a6,-(%sp)
        movea.l 44(%sp),%a0
        move.l  48(%sp),%d7
        add.l   %a0,%d7
        movea.l 52(%sp),%a2
        movea.l 56(%sp),%a3
        movea.l %a0,%a4
        move.l  %d7,%d6
        subq.l  #3,%d6
        movea.l %a3,%a1                 | no positions yet
        move.w  #4095,%d0
        moveq   #0,%d1
1:      move.l  %d1,(%a1)+
        dbra    %d0,1b
ploop:  cmpa.l  %d6,%a0
        bhi     ptail
        moveq   #0,%d0                  | the hash of q
        move.b  (%a0),%d0
        lsl.w   #6,%d0
        moveq   #0,%d1
        move.b  1(%a0),%d1
        lsl.w   #3,%d1
        eor.w   %d1,%d0
        moveq   #0,%d1
        move.b  2(%a0),%d1
        eor.w   %d1,%d0
        and.w   #0xFFF,%d0
        add.w   %d0,%d0
        add.w   %d0,%d0
        movea.l 0(%a3,%d0.w),%a1        | the candidate
        move.l  %a0,0(%a3,%d0.w)
        move.l  %a1,%d1
        beq     plit
        move.l  %a0,%d1
        sub.l   %a1,%d1                 | the offset
        cmp.l   #65535,%d1
        bhi     plit
        move.b  (%a1)+,%d2
        cmp.b   (%a0),%d2
        bne     plit
        move.b  (%a1)+,%d2
        cmp.b   1(%a0),%d2
        bne     plit
        move.b  (%a1)+,%d2
        cmp.b   2(%a0),%d2
        bne     plit
        lea     3(%a0),%a6              | as long as it matches
        move.l  %d7,%d2
        sub.l   %a6,%d2
        beq.s   3f
        subq.l  #1,%d2
2:      cmpm.b  (%a1)+,(%a6)+
        dbne    %d2,2b
        bne.s   4f
        sub.l   #0x10000,%d2            | (more than 65536 bytes)
        bpl.s   2b
        bra.s   3f
4:      subq.l  #1,%a6
3:      move.l  %a6,%d3
        sub.l   %a0,%d3                 | its length
        bsr     pemit
6:      movea.l %a6,%a0
        movea.l %a6,%a4
        bra     ploop
plit:   addq.l  #1,%a0
        bra     ploop
ptail:  movea.l %d7,%a0                 | the rest: literals
        cmpa.l  %a0,%a4
        bhs.s   7f
        moveq   #0,%d3
        bsr     pemit
7:      move.l  %a2,%d0
        sub.l   52(%sp),%d0
        movem.l (%sp)+,%d2-%d7/%a2-%a4/%a6
        rts

| a token: the literals [a4, a0), then (d3 > 0) a match of d3 bytes d1 back. a4 ends at a0.
pemit:  move.l  %a0,%d4
        sub.l   %a4,%d4                 | literals
        moveq   #15,%d0
        cmp.l   %d0,%d4
        bhs.s   1f
        move.b  %d4,%d0
1:      lsl.b   #4,%d0
        move.l  %d3,%d2
        beq.s   2f
        subq.l  #3,%d2
        moveq   #15,%d5
        cmp.l   %d5,%d2
        bls.s   3f
        moveq   #15,%d2
3:      or.b    %d2,%d0
2:      move.b  %d0,(%a2)+              | the token
        moveq   #15,%d0
        cmp.l   %d0,%d4
        blo.s   4f
        move.l  %d4,%d0
        sub.l   #15,%d0
        bsr.s   plen
4:      tst.l   %d4
        beq.s   6f
5:      move.b  (%a4)+,(%a2)+           | the literals
        subq.l  #1,%d4
        bne.s   5b
6:      tst.l   %d3
        beq.s   7f
        move.w  %d1,%d0                 | the offset
        lsr.w   #8,%d0
        move.b  %d0,(%a2)+
        move.b  %d1,(%a2)+
        move.l  %d3,%d0
        sub.l   #18,%d0                 | (length - 3 - 15)
        blo.s   7f
        bsr.s   plen
7:      rts

| a length past 15: 255s, then the rest (d0 >= 0)
plen:   cmp.l   #255,%d0
        blo.s   1f
        st      (%a2)+
        sub.l   #255,%d0
        bra.s   plen
1:      move.b  %d0,(%a2)+
        rts

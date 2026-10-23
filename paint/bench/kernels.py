"""Doodle's decoder kernels: 68000 assembly emitters shared by the benchmark (gen_bench.py)
and the app (paint/app/gen_kernels.py).

This file is executed in the caller's namespace (exec), so the multi-line assembly strings
stay byte-for-byte as they are. The caller must define:
  emit(str)        append a line of assembly
  G(name), P(name) operand text for a scalar/routine global and for a buffer pointer
                   (the benchmark uses A5-relative globals, the app absolute symbols)
  HS               hidden sizes for which LSTM cell copies are generated
  NOUT, NEMB       output rows (123) and embedding width (32)
Routines (see FORMAT.md): bld_4c, bld_pair, p_lut, mv_W, mv_B, mv_P (+ unrolled blocks), p_rec,
p_inp, gact, grun, gupd, p_gate, gate_gru, gate_lstm, lcell_<H>, p_hq, p_out, p_fold (single
shift, benchmark), smx, exps, topx, p_samp, step_prime, step_full. They need `rand`.
"""
# ---------------------------------------------------------------- LUT builders
emit("""| bld_4c: a0 -> 4 activation words, a3 -> 81-entry table (advanced past it).
| entry idx = sum d_j*3^j holds sum (d_j - 1)*x_j
bld_4c: movem.w (%a0),%d1-%d4
        move.w  %d1,%d6""")
for j in range(1, 4):
    emit(f"""        move.w  %d{j + 1},%d0
        sub.w   %d6,%d{j + 1}
        sub.w   %d6,%d{j + 1}
        add.w   %d0,%d6""")
emit("        move.w  %d6,%d0\n        neg.w   %d0")
for idx in range(81):
    emit("        move.w  %d0,(%a3)+")
    if idx < 80:
        dg = [(idx // 3 ** j) % 3 for j in range(4)]
        j = next(j for j in range(4) if dg[j] < 2)
        emit(f"        add.w   %d{1 if j == 0 else j + 1},%d0")
emit("        rts")
emit("""| bld_pair: a0 -> (h1, h2), a3 -> 256-entry table; entry (w1+8)*16+(w2+8) = w1*h1 + w2*h2
bld_pair:
        move.w  (%a0),%d4
        move.w  2(%a0),%d2
        move.w  %d4,%d0
        add.w   %d2,%d0
        lsl.w   #3,%d0
        neg.w   %d0
        move.w  %d2,%d3
        lsl.w   #4,%d3
        sub.w   %d2,%d3
        sub.w   %d3,%d4""")
for row in range(16):
    for col in range(16):
        emit("        move.w  %d0,(%a3)+")
        if col < 15:
            emit("        add.w   %d2,%d0")
        elif row < 15:
            emit("        add.w   %d4,%d0")
emit(f"""        rts
| p_lut: build the lookup tables for HQ (dispatch on storage kind)
p_lut:  movea.l {G('BLD')},%a0
        jmp     (%a0)
bld_t:  movea.l {P('HQ')},%a0
        movea.l {P('LUT')},%a3
        move.w  {G('G')},%d7
        subq.w  #1,%d7
1:      jsr     bld_4c
        addq.l  #8,%a0
        dbf     %d7,1b
        rts
bld_p:  movea.l {P('HQ')},%a0
        movea.l {P('LUT')},%a3
        move.w  {G('G')},%d7
        subq.w  #1,%d7
1:      jsr     bld_pair
        addq.l  #4,%a0
        dbf     %d7,1b
        rts
""")

# ---------------------------------------------------------------- matrix kernels
MAXG = max(HS) // 4
emit(f"""| mv_W (W4 and P4): a1 = codes, a6 = out words, d6 = rows-1. Entry into unrolled block.
mv_W:   movea.l {G('ENTRY')},%a4
        movea.l {P('A2')},%a2
        movea.l {P('A3')},%a3
1:      moveq   #0,%d1
        jsr     (%a4)
        move.w  %d1,(%a6)+
        dbf     %d6,1b
        rts
mv_B:   movea.l {G('ENTRY')},%a4
        movea.l {P('LUT')},%a3
        moveq   #0,%d0
1:      moveq   #0,%d1
        movea.l %a3,%a2
        jsr     (%a4)
        move.w  %d1,(%a6)+
        dbf     %d6,1b
        rts
| mv_P (P4): two 16-bit accumulators per row (the block's last 64 pairs go to d2), combined in
| 32 bits and clamped to int16, so a rare overflow saturates instead of wrapping.
mv_P:   movea.l {G('ENTRY')},%a4
        movea.l {P('A2')},%a2
        movea.l {P('A3')},%a3
        move.l  #32767,%d3
        move.l  #-32768,%d4
1:      moveq   #0,%d1
        moveq   #0,%d2
        jsr     (%a4)
        ext.l   %d1
        ext.l   %d2
        add.l   %d2,%d1
        cmp.l   %d3,%d1
        ble.s   2f
        move.l  %d3,%d1
2:      cmp.l   %d4,%d1
        bge.s   3f
        move.l  %d4,%d1
3:      move.w  %d1,(%a6)+
        dbf     %d6,1b
        rts""")
emit("mvW_blk:")
for _ in range(MAXG):
    emit("        move.w  (%a1)+,%d0\n        add.w   0(%a2,%d0.w),%d1")
emit("mvW_end: rts")
emit("mvB_blk:")
for _ in range(MAXG):
    emit("        move.b  (%a1)+,%d0\n        add.w   0(%a2,%d0.w),%d1\n        lea     162(%a2),%a2")
emit("mvB_end: rts")
emit("mvP_blk:")
for i in range(max(HS) // 2):
    reg = "%a3" if i < max(HS) // 2 - 128 else "%a2"
    acc = "%d2" if i >= max(HS) // 2 - 64 else "%d1"      # last 64 pairs: second accumulator
    emit(f"        move.w  (%a1)+,%d0\n        add.w   0({reg},%d0.w),{acc}")
emit("mvP_end: rts")

emit(f"""| p_rec: S = U * hq (all R rows)
p_rec:  movea.l {P('U')},%a1
        movea.l {P('S')},%a6
        move.w  {G('R')},%d6
        subq.w  #1,%d6
        movea.l {G('MV')},%a0
        jmp     (%a0)

| p_inp: CX[i] = ((WDX*dx + WDY*dy) >> SHX) + E + P[pen]   (32-bit)
p_inp:  movea.l {P('WDX')},%a0
        movea.l {P('WDY')},%a1
        movea.l {P('E')},%a2
        movea.l {P('P')},%a3
        move.w  {G('PEN')},%d0
        mulu.w  {G('R')},%d0
        add.l   %d0,%d0
        adda.l  %d0,%a3
        movea.l {P('CX')},%a6
        move.w  {G('DX')},%d4
        move.w  {G('DY')},%d5
        move.w  {G('SHX')},%d7
        move.w  {G('R')},%d6
        subq.w  #1,%d6
1:      move.w  (%a0)+,%d0
        muls.w  %d4,%d0
        move.w  (%a1)+,%d1
        muls.w  %d5,%d1
        add.l   %d1,%d0
        asr.l   %d7,%d0
        move.w  (%a2)+,%d1
        ext.l   %d1
        add.l   %d1,%d0
        move.w  (%a3)+,%d1
        ext.l   %d1
        add.l   %d1,%d0
        move.l  %d0,(%a6)+
        dbf     %d6,1b
        rts

| gact: a0 = S, a1 = M, a2 = CX, a6 = out, a3 = table centre, d6 = n-1, d5/d4 = clamp hi/lo,
| d3 = index shift. out = T[clamp(cx + ((S*m) >> 12)) >> d3]
gact:   move.w  {G('SHR')},%d7
1:      move.w  (%a0)+,%d0
        muls.w  (%a1)+,%d0
        asr.l   %d7,%d0
        add.l   (%a2)+,%d0
        cmp.l   %d5,%d0
        ble.s   2f
        move.l  %d5,%d0
2:      cmp.l   %d4,%d0
        bge.s   3f
        move.l  %d4,%d0
3:      asr.w   %d3,%d0
        add.w   %d0,%d0
        move.w  0(%a3,%d0.w),(%a6)+
        dbf     %d6,1b
        rts

| grun: GRU candidate rows. u = clamp16(((S*m) >> 12) + bhn); n = TANH[clamp(cx + ((r*u) >> 14))]
grun:   move.w  {G('SHR')},%d7
        move.l  #32767,%d5
        move.l  #-32768,%d4
        move.l  #4095,%d3
        move.l  #-4096,%d2
1:      move.w  (%a0)+,%d0
        muls.w  (%a1)+,%d0
        asr.l   %d7,%d0
        move.w  (%a1)+,%d1
        ext.l   %d1
        add.l   %d1,%d0
        cmp.l   %d5,%d0
        ble.s   2f
        move.l  %d5,%d0
2:      cmp.l   %d4,%d0
        bge.s   3f
        move.l  %d4,%d0
3:      muls.w  (%a3)+,%d0
        lsl.l   #2,%d0
        swap    %d0
        ext.l   %d0
        add.l   (%a2)+,%d0
        cmp.l   %d3,%d0
        ble.s   4f
        move.l  %d3,%d0
4:      cmp.l   %d2,%d0
        bge.s   5f
        move.l  %d2,%d0
5:      asr.w   #3,%d0
        add.w   %d0,%d0
        move.w  0(%a4,%d0.w),(%a6)+
        dbf     %d6,1b
        rts

| gupd: h' = n + ((z*(h-n)) >> 14); hq = (h'*A) >> 14
gupd:   movea.l {P('GV_1')},%a0
        movea.l {P('GV_2')},%a1
        movea.l {P('HST')},%a2
        movea.l {P('HQ')},%a3
        move.w  {G('A')},%d5
        move.w  {G('H')},%d6
        subq.w  #1,%d6
1:      move.w  (%a1)+,%d1
        move.w  (%a2),%d0
        sub.w   %d1,%d0
        muls.w  (%a0)+,%d0
        lsl.l   #2,%d0
        swap    %d0
        add.w   %d1,%d0
        move.w  %d0,(%a2)+
        muls.w  %d5,%d0
        lsl.l   #2,%d0
        swap    %d0
        move.w  %d0,(%a3)+
        dbf     %d6,1b
        rts

| p_gate: gates + state update (dispatch on cell)
p_gate: movea.l {G('GATE')},%a0
        jmp     (%a0)
gate_gru:
        movea.l {P('S')},%a0
        movea.l {P('M')},%a1
        movea.l {P('CX')},%a2
        movea.l {P('GV')},%a6
        movea.l {G('SIGC')},%a3
        move.w  {G('H')},%d6
        add.w   %d6,%d6
        subq.w  #1,%d6
        move.l  #8191,%d5
        move.l  #-8192,%d4
        moveq   #4,%d3
        jsr     gact
        movea.l {P('S_2')},%a0
        movea.l {P('MN')},%a1
        movea.l {P('CX_2')},%a2
        movea.l {P('GV')},%a3
        movea.l {G('TANHC')},%a4
        movea.l {P('GV_2')},%a6
        move.w  {G('H')},%d6
        subq.w  #1,%d6
        jsr     grun
        bra     gupd
gate_lstm:
        movea.l {P('S')},%a0
        movea.l {P('M')},%a1
        movea.l {P('CX')},%a2
        movea.l {P('GV')},%a6
        movea.l {G('SIGC')},%a3
        move.w  {G('H')},%d6
        mulu.w  #3,%d6
        subq.w  #1,%d6
        move.l  #8191,%d5
        move.l  #-8192,%d4
        moveq   #4,%d3
        jsr     gact
        movea.l {P('S_3')},%a0
        movea.l {P('M_3')},%a1
        movea.l {P('CX_3')},%a2
        movea.l {P('GV_3')},%a6
        movea.l {G('TANHC')},%a3
        move.w  {G('H')},%d6
        subq.w  #1,%d6
        move.l  #4095,%d5
        move.l  #-4096,%d4
        moveq   #3,%d3
        jsr     gact
        movea.l {P('GV')},%a0
        movea.l {P('C')},%a4
        movea.l {P('HST')},%a6
        movea.l {P('HQ')},%a3
        movea.l {G('TANHC')},%a2
        move.w  {G('A')},%d5
        move.w  {G('H')},%d6
        subq.w  #1,%d6
        move.l  #32767,%d3
        move.l  #-32768,%d4
        move.w  #16383,%d2
        move.w  #-16384,%d7
        movea.l {G('LCELL')},%a1
        jmp     (%a1)""")

# LSTM cell, one copy per H (displacements to the f, o, g rows)
for hi, H in enumerate(HS):
    HB = 2 * H
    emit(f"""| lcell_{H}: c' = clamp16(((f*c) >> 14) + ((i*g) >> 16)); h' = (o*TANH[c']) >> 14; hq = (h'*A) >> 14
lcell_{H}:
1:      move.w  {HB}(%a0),%d0
        muls.w  (%a4),%d0
        lsl.l   #2,%d0
        swap    %d0
        ext.l   %d0
        move.w  (%a0),%d1
        muls.w  {3 * HB}(%a0),%d1
        swap    %d1
        ext.l   %d1
        add.l   %d1,%d0
        cmp.l   %d3,%d0
        ble.s   2f
        move.l  %d3,%d0
2:      cmp.l   %d4,%d0
        bge.s   3f
        move.l  %d4,%d0
3:      move.w  %d0,(%a4)+
        cmp.w   %d2,%d0
        ble.s   4f
        move.w  %d2,%d0
4:      cmp.w   %d7,%d0
        bge.s   5f
        move.w  %d7,%d0
5:      asr.w   #5,%d0
        add.w   %d0,%d0
        move.w  0(%a2,%d0.w),%d0
        muls.w  {2 * HB}(%a0),%d0
        lsl.l   #2,%d0
        swap    %d0
        move.w  %d0,(%a6)+
        muls.w  %d5,%d0
        lsl.l   #2,%d0
        swap    %d0
        move.w  %d0,(%a3)+
        addq.l  #2,%a0
        dbf     %d6,1b
        rts""")

emit(f"""| p_hq: HQ = (HST*A) >> 14 (initialisation only)
p_hq:   movea.l {P('HST')},%a0
        movea.l {P('HQ')},%a3
        move.w  {G('A')},%d5
        move.w  {G('H')},%d6
        subq.w  #1,%d6
1:      move.w  (%a0)+,%d0
        muls.w  %d5,%d0
        lsl.l   #2,%d0
        swap    %d0
        move.w  %d0,(%a3)+
        dbf     %d6,1b
        rts

| p_out: SY = WY * hq; O = clamp16(BY + ((SY*MY) >> 12))
p_out:  movea.l {P('WY')},%a1
        movea.l {P('SY')},%a6
        moveq   #{NOUT - 1},%d6
        movea.l {G('MV')},%a0
        jsr     (%a0)
        movea.l {P('SY')},%a0
        movea.l {P('MY')},%a1
        movea.l {P('BY')},%a2
        movea.l {P('O')},%a6
        moveq   #{NOUT - 1},%d6
        move.w  {G('SHO')},%d7
        move.l  #32767,%d5
        move.l  #-32768,%d4
1:      move.w  (%a0)+,%d0
        muls.w  (%a1)+,%d0
        asr.l   %d7,%d0
        move.w  (%a2)+,%d1
        ext.l   %d1
        add.l   %d1,%d0
        cmp.l   %d5,%d0
        ble.s   2f
        move.l  %d5,%d0
2:      cmp.l   %d4,%d0
        bge.s   3f
        move.l  %d4,%d0
3:      move.w  %d0,(%a6)+
        dbf     %d6,1b
        rts

| p_fold: E = clamp16(B0 + ((WE * emb[cat]) >> 7)), WE and the embedding table are bytes
p_fold: movea.l {P('EMB')},%a0
        move.w  {G('CAT')},%d0
        lsl.w   #5,%d0
        adda.w  %d0,%a0
        movea.l {P('EW')},%a1
        moveq   #{NEMB - 1},%d6
1:      move.b  (%a0)+,%d0
        ext.w   %d0
        move.w  %d0,(%a1)+
        dbf     %d6,1b
        movea.l {P('WE')},%a0
        movea.l {P('B0')},%a2
        movea.l {P('E')},%a6
        move.w  {G('R')},%d6
        subq.w  #1,%d6
        move.l  #32767,%d5
        move.l  #-32768,%d4
2:      movea.l {P('EW')},%a1
        moveq   #0,%d1""")
for _ in range(NEMB):
    emit("        move.b  (%a0)+,%d0\n        ext.w   %d0\n        muls.w  (%a1)+,%d0\n        add.l   %d0,%d1")
emit(f"""        asr.l   #7,%d1
        move.w  (%a2)+,%d0
        ext.l   %d0
        add.l   %d0,%d1
        cmp.l   %d5,%d1
        ble.s   3f
        move.l  %d5,%d1
3:      cmp.l   %d4,%d1
        bge.s   4f
        move.l  %d4,%d1
4:      move.w  %d1,(%a6)+
        dbf     %d6,2b
        rts

| smx: a0 = n logits (words), d6 = n-1 -> d0 = sampled index. Temperature via INVT (Q8),
| probabilities EXPP[min((max - l) >> 5, 511)] (Q11), pick with 16 random bits.
smx:    move.w  {G('INVT')},%d3
        movea.l {P('TL')},%a1
        move.l  #0x80000000,%d2
        move.w  %d6,%d5
1:      move.w  (%a0)+,%d0
        muls.w  %d3,%d0
        asr.l   #8,%d0
        move.l  %d0,(%a1)+
        cmp.l   %d2,%d0
        ble.s   2f
        move.l  %d0,%d2
2:      dbf     %d5,1b
        movea.l {P('TL')},%a1
        movea.l %a1,%a2
        movea.l {G('EXPP')},%a3
        moveq   #0,%d4
        move.w  %d6,%d5
3:      move.l  %d2,%d0
        sub.l   (%a1)+,%d0
        asr.l   #5,%d0
        cmp.l   #511,%d0
        bls.s   4f
        move.w  #511,%d0
4:      add.w   %d0,%d0
        move.w  0(%a3,%d0.w),%d0
        move.w  %d0,(%a2)+
        add.w   %d0,%d4
        dbf     %d5,3b
        jsr     rand
        mulu.w  %d4,%d0
        swap    %d0
        movea.l {P('TL')},%a2
        moveq   #0,%d1
        moveq   #0,%d2
5:      add.w   (%a2)+,%d1
        cmp.w   %d1,%d0
        bcs.s   6f
        addq.w  #1,%d2
        cmp.w   %d6,%d2
        bls.s   5b
        move.w  %d6,%d2
6:      move.w  %d2,%d0
        rts

| exps: d0.w = log sigma (Q10) -> d0.w = sigma*sqrt(temperature) (Q10) = EXPS[clamp(raw + HLT)]
exps:   ext.l   %d0
        move.w  {G('HLT')},%d1
        ext.l   %d1
        add.l   %d1,%d0
        cmp.l   #4095,%d0
        ble.s   1f
        move.l  #4095,%d0
1:      cmp.l   #-8192,%d0
        bge.s   2f
        move.l  #-8192,%d0
2:      asr.w   #4,%d0
        add.w   %d0,%d0
        movea.l {G('EXPSC')},%a0
        move.w  0(%a0,%d0.w),%d0
        rts

| topx: d0.l = offset (Q10) -> d0.w = pixels: clamp16, (x*FQ + 32768) >> 16, clamp +-255
topx:   cmp.l   #32767,%d0
        ble.s   1f
        move.l  #32767,%d0
1:      cmp.l   #-32768,%d0
        bge.s   2f
        move.l  #-32768,%d0
2:      muls.w  {G('FQ')},%d0
        add.l   #32768,%d0
        swap    %d0
        cmp.w   #255,%d0
        ble.s   3f
        move.w  #255,%d0
3:      cmp.w   #-255,%d0
        bge.s   4f
        move.w  #-255,%d0
4:      rts

| p_samp: sample the next pen move from O (pi, mu_x, mu_y, log sigma_x, log sigma_y, rho: 20 each; then 3 pen logits). SAMP: k, pen, dx, dy, -, -, s1, s2, rho, c, n1, n2, x.l, y.l
p_samp: movea.l {P('O')},%a0
        moveq   #19,%d6
        jsr     smx
        movea.l {P('SAMP')},%a6
        move.w  %d0,(%a6)
        movea.l {P('O')},%a4
        add.w   %d0,%d0
        adda.w  %d0,%a4
        move.w  120(%a4),%d0
        jsr     exps
        move.w  %d0,12(%a6)
        move.w  160(%a4),%d0
        jsr     exps
        move.w  %d0,14(%a6)
        move.w  200(%a4),%d0
        cmp.w   #4095,%d0
        ble.s   1f
        move.w  #4095,%d0
1:      cmp.w   #-4096,%d0
        bge.s   2f
        move.w  #-4096,%d0
2:      asr.w   #3,%d0
        add.w   %d0,%d0
        movea.l {G('TANHC')},%a0
        move.w  0(%a0,%d0.w),16(%a6)
        movea.l {G('SECHC')},%a0
        move.w  0(%a0,%d0.w),18(%a6)
        jsr     rand
        move.l  %d0,%d1
        moveq   #22,%d2
        lsr.l   %d2,%d0
        add.w   %d0,%d0
        movea.l {G('RTAB')},%a0
        move.w  0(%a0,%d0.w),%d3
        move.l  %d1,%d0
        moveq   #12,%d2
        lsr.l   %d2,%d0
        and.w   #1023,%d0
        move.w  %d0,%d2
        add.w   #768,%d2
        and.w   #1023,%d2
        add.w   %d0,%d0
        add.w   %d2,%d2
        movea.l {G('COS')},%a0
        move.w  0(%a0,%d0.w),%d0
        move.w  0(%a0,%d2.w),%d2
        muls.w  %d3,%d0
        lsl.l   #2,%d0
        swap    %d0
        muls.w  %d3,%d2
        lsl.l   #2,%d2
        swap    %d2
        move.w  %d0,20(%a6)
        move.w  %d2,22(%a6)
        muls.w  12(%a6),%d0
        moveq   #12,%d7
        asr.l   %d7,%d0
        move.w  40(%a4),%d1
        ext.l   %d1
        add.l   %d1,%d0
        move.l  %d0,24(%a6)
        move.w  20(%a6),%d0
        muls.w  16(%a6),%d0
        lsl.l   #2,%d0
        swap    %d0
        ext.l   %d0
        muls.w  18(%a6),%d2
        lsl.l   #2,%d2
        swap    %d2
        ext.l   %d2
        add.l   %d2,%d0
        muls.w  14(%a6),%d0
        asr.l   %d7,%d0
        move.w  80(%a4),%d1
        ext.l   %d1
        add.l   %d1,%d0
        move.l  %d0,28(%a6)
        movea.l {P('O')},%a0
        lea     240(%a0),%a0
        moveq   #2,%d6
        move.w  {G('INVT')},-(%sp)              | the pen pick uses its own temperature (INVTP)
        move.w  {G('INVTP')},{G('INVT')}
        jsr     smx
        move.w  (%sp)+,{G('INVT')}
        movea.l {P('SAMP')},%a6
        move.w  %d0,2(%a6)
        move.w  %d0,{G('PEN')}
        move.l  24(%a6),%d0
        jsr     topx
        move.w  %d0,4(%a6)
        move.w  %d0,{G('DX')}
        move.l  28(%a6),%d0
        jsr     topx
        move.w  %d0,6(%a6)
        move.w  %d0,{G('DY')}
        rts

| one decoder step while priming (no output layer / sampling) and a full pen move
step_prime:
        jsr     p_rec
        jsr     p_inp
        jsr     p_gate
        jmp     p_lut
step_full:
        jsr     p_rec
        jsr     p_inp
        jsr     p_gate
        jsr     p_lut
        jsr     p_out
        jmp     p_samp
""")


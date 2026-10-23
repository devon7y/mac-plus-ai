#!/usr/bin/env python3
"""Generate Doodle's "Finish it" decoder benchmark for the 68000 (Mac Plus, 1 MB).

Writes bench.s (position-independent 68000 assembly) and layout.json. The program
  1. records the memory a program gets (FreeMem, MaxMem, BufPtr - ApplZone) and tries to
     allocate each configuration's full runtime footprint (fit test);
  2. for every configuration (cell x hidden size H x weight storage), fills the model with
     random weights, runs one full decoder step and an 8-step sampled trace, and hands
     inputs, intermediates and outputs to bench.lua (dump handshake) for verify.py;
  3. times each part of a step with the Mac's tick counter (60.15 Hz).

Model per step (see FORMAT.md): input = stroke-5 point (dx, dy, pen one-hot) + 32-wide
category embedding (folded into a bias once per drawing), recurrent cell GRU (3H rows) or
LSTM (4H rows) over hidden size H, output layer H -> 123 (20-component bivariate Gaussian
mixture + 3 pen states), sampling with temperature.

Weight storage variants (both recurrent and output matrices):
  W4  ternary, groups of 4, 16-bit codes (4 bits/weight in RAM)
  B4  ternary, groups of 4, byte codes   (2 bits/weight in RAM)
  P4  4-bit weights, pairs, 16-bit codes (8 bits/weight in RAM; 4 bits on disk)
"""
import json
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tables import CENTER, TABLES  # noqa: E402

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
HS = (192, 256, 384)
KINDS = ("W4", "B4", "P4")
CELLS = ("gru", "lstm")
NOUT, NEMB, NCAT = 123, 32, 345
TRACE_N = 8
TARGET_S = 2.5                       # emulated seconds per timing measurement
CLOCK, SLOWDOWN = 7833600, 1.30      # 68000 clock; video/interrupt overhead factor (estimate)
PARTS = ("fold", "lut", "rec", "inp", "gate", "out", "samp", "prim", "step")
RES_SIZE = 8192

CONFIGS = [(c, h, k) for c in CELLS for h in HS for k in KINDS]
RUN = [cfg for cfg in CONFIGS if cfg != ("lstm", 384, "P4")]      # too big to run alongside the bench


def rows_of(cell, H):
    return 3 * H if cell == "gru" else 4 * H


def groups(kind, H):
    return H // 4 if kind in ("W4", "B4") else H // 2


def code_bytes(kind, rows, H):
    return rows * groups(kind, H) * (1 if kind == "B4" else 2)


def lut_bytes(kind, H):
    return groups(kind, H) * (162 if kind != "P4" else 512)


# ---------------------------------------------------------------- buffer layout per config
PTRS = ["U", "WY", "LUT", "A2", "A3", "M", "MN", "MY", "BY", "E", "B0", "P", "WDX", "WDY", "WE", "EMB",
        "EW", "CX", "S", "GV", "HST", "HQ", "C", "SY", "O", "TL", "SAMP", "SCAL", "TRACE",
        "S_2", "CX_2", "GV_1", "GV_2", "S_3", "M_3", "CX_3", "GV_3"]
PIDX = {n: i for i, n in enumerate(PTRS)}


def config_layout(cell, H, kind):
    R = rows_of(cell, H)
    lay, cur = {}, RES_SIZE

    def alloc(name, size):
        nonlocal cur
        lay[name] = (cur, size)
        cur += (size + 3) & ~3

    alloc("U", code_bytes(kind, R, H))
    alloc("WY", code_bytes(kind, NOUT, H))
    alloc("LUT", lut_bytes(kind, H))
    alloc("M", 2 * (2 * H if cell == "gru" else 4 * H))
    alloc("MN", 2 * (2 * H if cell == "gru" else 0))
    alloc("MY", 2 * NOUT)
    alloc("BY", 2 * NOUT)
    alloc("E", 2 * R)
    alloc("B0", 2 * R)
    alloc("P", 2 * 3 * R)
    alloc("WDX", 2 * R)
    alloc("WDY", 2 * R)
    alloc("WE", R * NEMB)
    alloc("EMB", NCAT * NEMB)
    alloc("EW", 2 * NEMB)
    alloc("CX", 4 * R)
    alloc("S", 2 * R)
    alloc("GV", 2 * R)
    alloc("HST", 2 * H)
    alloc("HQ", 2 * H)
    alloc("C", 2 * H)
    alloc("SY", 2 * NOUT)
    alloc("O", 2 * NOUT)
    alloc("TL", 4 * 20)
    alloc("SAMP", 32)
    alloc("SCAL", 24)
    alloc("TRACE", 32 * TRACE_N)
    off = {n: o for n, (o, _) in lay.items()}
    lut = off["LUT"]
    ptr = dict(off)
    ptr["A2"] = lut if kind != "P4" else lut + 32768
    ptr["A3"] = lut + 65536 + 32768 if kind == "P4" else lut
    ptr["S_2"], ptr["CX_2"] = off["S"] + 4 * H, off["CX"] + 8 * H
    ptr["GV_1"], ptr["GV_2"] = off["GV"] + 2 * H, off["GV"] + 4 * H
    ptr["S_3"], ptr["M_3"] = off["S"] + 6 * H, off["M"] + 6 * H
    ptr["CX_3"], ptr["GV_3"] = off["CX"] + 12 * H, off["GV"] + 6 * H
    return lay, ptr, cur


def footprint(cell, H, kind):
    """Bytes the real app needs for this model at run time: weights, lookup tables, per-row
    constants, embedding table, state vectors and the fixed-point tables (12.8 KB)."""
    lay, _, _ = config_layout(cell, H, kind)
    skip = {"TRACE", "SAMP", "SCAL", "TL"}
    return sum((s + 3) & ~3 for n, (_, s) in lay.items() if n not in skip) + 2 * sum(len(t) for t in TABLES.values()) + 256


# ---------------------------------------------------------------- cost model (only for repeat counts)
def est_cycles(cell, H, kind):
    R, G = rows_of(cell, H), groups(kind, H)
    c = {"W4": 22, "B4": 30, "P4": 22}[kind]
    e = {}
    e["fold"] = R * (32 * 62 + 90)
    e["lut"] = G * (81 * 12 + 70) if kind != "P4" else G * (256 * 12 + 60)
    e["rec"] = R * (G * c + 54)
    e["inp"] = R * 190
    e["gate"] = (2 * H * 150 + H * 240 + H * 130) if cell == "gru" else (4 * H * 150 + H * 260)
    e["out"] = NOUT * (G * c + 54) + NOUT * 90
    e["samp"] = 25000
    e["prim"] = e["lut"] + e["rec"] + e["inp"] + e["gate"]
    e["step"] = e["prim"] + e["out"] + e["samp"]
    return e


def reps_for(cell, H, kind):
    e = est_cycles(cell, H, kind)
    out = {}
    for p in PARTS:
        if cell == "lstm" and p in ("fold", "lut", "inp", "out", "samp"):
            out[p] = 0                       # identical to the GRU's (or scales trivially)
            continue
        s = e[p] * SLOWDOWN / CLOCK
        out[p] = max(2, min(5000, math.ceil(TARGET_S / s)))
    return out


# ---------------------------------------------------------------- globals (below A5)
GL, _g = {}, [0]


def gdef(name, size):
    _g[0] -= size
    GL[name] = _g[0]


for n in ("MAIN", "SEED", "TI_R", "TI_N", "TI_T0", "TI_TAG", "DESC", "ENTRY", "MV", "BLD", "GATE",
          "LCELL", "SIGC", "TANHC", "SECHC", "EXPSC", "EXPP", "RTAB", "COS", "TRP"):
    gdef(n, 4)
for n in ("H", "G", "R", "A", "SHR", "SHO", "SHX", "PEN", "DX", "DY", "CAT", "INVT", "INVTP", "HLT", "FQ", "CFG", "PI", "TRN", "DID"):
    gdef(n, 2)
GPTR = -512                                  # pointer array: GPTR + 4*i
assert _g[0] > GPTR + 4


def G(name):
    return f"{GL[name]}(%a5)"


def P(name):
    return f"{GPTR + 4 * PIDX[name]}(%a5)"


def fourcc(s):
    return "0x" + s.encode().hex()


# ---------------------------------------------------------------- descriptor format
DFIELDS = ["id", "kind", "cell", "H", "G", "R", "A", "na3", "na2", "eback", "hsel", "nm", "nmn"]
DOFF = {f: 4 * i for i, f in enumerate(DFIELDS)}
DOFF["reps"] = 4 * len(DFIELDS)
DOFF["ptrs"] = DOFF["reps"] + 4 * len(PARTS)
DOFF["dumps"] = DOFF["ptrs"] + 4 * len(PTRS)
assert DOFF["reps"] + 4 * len(PARTS) < 128

# dump phases: (phase name, [(item name, pointer, size function)])
def dump_plan(cell, H, kind):
    lay, _, _ = config_layout(cell, H, kind)
    sz = {n: s for n, (_, s) in lay.items()}
    pre = ["U", "WY", "M", "MN", "MY", "BY", "WE", "B0", "EMB", "E", "P", "WDX", "WDY", "HST", "HQ", "C", "SCAL"]
    post = ["S", "CX", "GV", "HST", "HQ", "C", "SY", "O", "SAMP", "SCAL"]
    trace = ["TRACE", "HST", "HQ", "C", "SCAL"]
    return [[(n, sz[n]) for n in ph] for ph in (pre, post, trace)]


# ---------------------------------------------------------------- code
asm = []
emit = asm.append

emit(f"""| Generated by paint/bench/gen_bench.py. 68000, position-independent, no relocations.
        .text
        .globl _start
_start:
        .short  0xA063                  | _MaxApplZone
        .short  0xA01C                  | _FreeMem -> d0
        move.l  %d0,-(%sp)
        .short  0xA11D                  | _MaxMem -> d0
        move.l  %d0,-(%sp)
        move.l  (0x10C).w,%d0           | BufPtr
        sub.l   (0x2AA).w,%d0           | - ApplZone
        move.l  %d0,-(%sp)""")

# fit tests (before the main allocation)
FITS = [(i, footprint(*cfg)) for i, cfg in enumerate(CONFIGS)]
for i, size in FITS:
    emit(f"""        move.l  #{size},%d0
        .short  0xA11E                  | _NewPtr
        moveq   #0,%d5
        move.l  %a0,%d0
        beq.s   1f
        moveq   #1,%d5
        .short  0xA01F                  | _DisposPtr
1:      move.w  %d5,-(%sp)""")

LAYS = {cfg: config_layout(*cfg) for cfg in RUN}
MAIN = max(l[2] for l in LAYS.values())
emit(f"""        move.l  #{MAIN},%d0
        .short  0xA11E                  | _NewPtr
        move.l  %a0,%d0
        bne.s   2f
3:      bra.s   3b                      | out of memory: hang
2:      move.l  %a0,{G('MAIN')}
        move.l  #0x2545F491,{G('SEED')}
        move.l  #0x504E5442,(%a0)       | 'PNTB'
        clr.l   4(%a0)                  | status: running
        clr.l   8(%a0)                  | entries
        clr.l   12(%a0)                 | dump requests
        jsr     set_tables""")
for i, size in reversed(FITS):
    emit(f"""        moveq   #0,%d2
        move.w  (%sp)+,%d2
        move.l  #{fourcc('FIT ')},%d1
        move.l  #{size},%d3
        moveq   #{i},%d4
        jsr     rec""")
emit(f"""        move.l  #{fourcc('MEM ')},%d1
        move.l  (%sp)+,%d4              | BufPtr - ApplZone
        move.l  (%sp)+,%d3              | MaxMem
        move.l  (%sp)+,%d2              | FreeMem
        jsr     rec
        lea     _start(%pc),%a0
        lea     code_end(%pc),%a1
        move.l  %a1,%d2
        sub.l   %a0,%d2
        move.l  #{fourcc('CODE')},%d1
        move.l  #{MAIN},%d3
        moveq   #0,%d4
        jsr     rec
        lea     cfg_list(%pc),%a3
        move.w  (%a3)+,%d7
        subq.w  #1,%d7
4:      move.w  %d7,-(%sp)
        move.l  %a3,-(%sp)
        lea     cfg_list(%pc),%a0
        adda.w  (%a3),%a0
        jsr     run_cfg
        movea.l (%sp)+,%a3
        addq.l  #2,%a3
        move.w  (%sp)+,%d7
        dbf     %d7,4b
        movea.l {G('MAIN')},%a0
        move.l  #1,4(%a0)               | status: done
done:   bra.s   done
""")

emit(f"""| ---------------------------------------------------------------- helpers
| rec: append (d1 tag, d2, d3, d4) to the results block
rec:    movea.l {G('MAIN')},%a0
        move.l  8(%a0),%d0
        addq.l  #1,8(%a0)
        lsl.l   #4,%d0
        adda.l  %d0,%a0
        lea     512(%a0),%a0
        movem.l %d1-%d4,(%a0)
        rts

| adddump: d2 = address, d3 = size, d4 = id
adddump:
        movea.l {G('MAIN')},%a0
        move.l  12(%a0),%d0
        addq.l  #1,12(%a0)
        mulu.w  #12,%d0
        lea     16(%a0,%d0.l),%a0
        movem.l %d2-%d4,(%a0)
        rts

| flush: ask bench.lua to copy the requested buffers out, wait until it has
flush:  movea.l {G('MAIN')},%a0
        move.l  #2,4(%a0)
1:      cmpi.l  #2,4(%a0)
        beq.s   1b
        rts

| timeit: a0 = routine, d1 = tag, d2 = repeats. Records (tag, ticks, repeats, config).
timeit: move.l  %a0,{G('TI_R')}
        move.l  %d1,{G('TI_TAG')}
        move.l  %d2,{G('TI_N')}
        move.l  (0x16A).w,{G('TI_T0')}
1:      movea.l {G('TI_R')},%a0
        jsr     (%a0)
        subq.l  #1,{G('TI_N')}
        bne.s   1b
        move.l  (0x16A).w,%d2
        sub.l   {G('TI_T0')},%d2
        move.l  {G('TI_TAG')},%d1
        movea.l {G('DESC')},%a0
        move.w  {G('PI')},%d0
        lsl.w   #2,%d0
        move.l  {DOFF['reps']}(%a0,%d0.w),%d3
        moveq   #0,%d4
        move.w  {G('CFG')},%d4
        jsr     rec
        rts

| rand: xorshift32 (x ^= x<<13; x ^= x>>17; x ^= x<<5) -> d0. Preserves all but d0.
rand:   move.l  %d1,-(%sp)
        move.l  {G('SEED')},%d0
        move.l  %d0,%d1
        lsl.l   #8,%d1
        lsl.l   #5,%d1
        eor.l   %d1,%d0
        move.l  %d0,%d1
        swap    %d1
        and.l   #0xFFFF,%d1
        lsr.l   #1,%d1
        eor.l   %d1,%d0
        move.l  %d0,%d1
        lsl.l   #5,%d1
        eor.l   %d1,%d0
        move.l  %d0,{G('SEED')}
        move.l  (%sp)+,%d1
        rts

| fill_u: a0 = words, d6 = n-1, d5 = span, d4 = lo: uniform in [lo, lo+span)
fill_u: jsr     rand
        mulu.w  %d5,%d0
        swap    %d0
        add.w   %d4,%d0
        move.w  %d0,(%a0)+
        dbf     %d6,fill_u
        rts
fill_ub:
        jsr     rand
        mulu.w  %d5,%d0
        swap    %d0
        add.w   %d4,%d0
        move.b  %d0,(%a0)+
        dbf     %d6,fill_ub
        rts

| fill_codes: a0 = codes, d6 = rows-1; kind/G/segments from the descriptor (a4)
fill_codes:
        move.l  {DOFF['G']}(%a4),%d5
        move.l  {DOFF['kind']}(%a4),%d0
        beq.s   fill_w4
        subq.l  #1,%d0
        beq.s   fill_b4
        move.l  {DOFF['na3']}(%a4),%d5
        move.l  {DOFF['na2']}(%a4),%d7
        bra     fill_p4
| W4: 16-bit code (g*81 + idx)*2, idx uniform 0..80
fill_w4:
1:      moveq   #0,%d3
        move.w  %d5,%d4
        subq.w  #1,%d4
2:      jsr     rand
        mulu.w  #81,%d0
        swap    %d0
        add.w   %d0,%d0
        add.w   %d3,%d0
        move.w  %d0,(%a0)+
        add.w   #162,%d3
        dbf     %d4,2b
        dbf     %d6,1b
        rts
| B4: byte code idx*2
fill_b4:
1:      move.w  %d5,%d4
        subq.w  #1,%d4
2:      jsr     rand
        mulu.w  #81,%d0
        swap    %d0
        add.b   %d0,%d0
        move.b  %d0,(%a0)+
        dbf     %d4,2b
        dbf     %d6,1b
        rts
| P4: per row d5 pairs in the a3 window, then d7 pairs in the a2 window; code (p*256+c)*2-32768
fill_p4:
1:      move.w  %d5,%d4
        beq.s   2f
        jsr     pseg
2:      move.w  %d7,%d4
        jsr     pseg
        dbf     %d6,1b
        rts
pseg:   subq.w  #1,%d4
        move.w  #-32768,%d3
1:      jsr     rand
        and.w   #255,%d0
        add.w   %d0,%d0
        add.w   %d3,%d0
        move.w  %d0,(%a0)+
        add.w   #512,%d3
        dbf     %d4,1b
        rts

| snap: copy the scalar state into SCAL (seed.l pen dx dy cat invt hlt fq A shr sho)
snap:   movea.l {P('SCAL')},%a0
        move.l  {G('SEED')},(%a0)+
        move.w  {G('PEN')},(%a0)+
        move.w  {G('DX')},(%a0)+
        move.w  {G('DY')},(%a0)+
        move.w  {G('CAT')},(%a0)+
        move.w  {G('INVT')},(%a0)+
        move.w  {G('HLT')},(%a0)+
        move.w  {G('FQ')},(%a0)+
        move.w  {G('A')},(%a0)+
        move.w  {G('SHR')},(%a0)+
        move.w  {G('SHO')},(%a0)+
        rts
""")

# ---------------------------------------------------------------- kernels (shared with the app)
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "kernels.py")).read())

# ---------------------------------------------------------------- per-config driver
emit(f"""| run_cfg: a0 = descriptor
run_cfg:
        move.l  %a0,{G('DESC')}
        movea.l %a0,%a4
        lea     {GPTR}(%a5),%a1
        lea     {DOFF['ptrs']}(%a4),%a2
        move.w  #{len(PTRS) - 1},%d6
1:      move.l  {G('MAIN')},%d0
        add.l   (%a2)+,%d0
        move.l  %d0,(%a1)+
        dbf     %d6,1b
        move.w  {DOFF['id'] + 2}(%a4),{G('CFG')}
        move.w  {DOFF['H'] + 2}(%a4),{G('H')}
        move.w  {DOFF['G'] + 2}(%a4),{G('G')}
        move.w  {DOFF['R'] + 2}(%a4),{G('R')}
        move.w  {DOFF['A'] + 2}(%a4),{G('A')}
        move.w  #12,{G('SHR')}
        move.w  #12,{G('SHO')}
        move.w  #6,{G('SHX')}
        move.w  #0,{G('PEN')}
        move.w  #7,{G('DX')}
        move.w  #-3,{G('DY')}
        move.w  #123,{G('CAT')}
        move.w  #512,{G('INVT')}
        move.w  #512,{G('INVTP')}
        move.w  #-355,{G('HLT')}
        move.w  #1536,{G('FQ')}
        move.w  {DOFF['id'] + 2}(%a4),%d0
        lsl.w   #6,%d0
        move.w  %d0,{G('DID')}
| storage kind -> builder, kernel, entry point
        move.l  {DOFF['kind']}(%a4),%d0
        lea     bld_t(%pc),%a1
        lea     mv_W(%pc),%a2
        lea     mvW_end(%pc),%a3
        tst.l   %d0
        beq.s   2f
        lea     mv_B(%pc),%a2
        lea     mvB_end(%pc),%a3
        subq.l  #1,%d0
        beq.s   2f
        lea     bld_p(%pc),%a1
        lea     mv_P(%pc),%a2
        lea     mvP_end(%pc),%a3
2:      suba.l  {DOFF['eback']}(%a4),%a3
        move.l  %a1,{G('BLD')}
        move.l  %a2,{G('MV')}
        move.l  %a3,{G('ENTRY')}
        lea     gate_gru(%pc),%a1
        tst.l   {DOFF['cell']}(%a4)
        beq.s   3f
        lea     gate_lstm(%pc),%a1
3:      move.l  %a1,{G('GATE')}
        move.l  {DOFF['hsel']}(%a4),%d0
        lea     lcell_{HS[0]}(%pc),%a1
        beq.s   4f
        lea     lcell_{HS[1]}(%pc),%a1
        subq.l  #1,%d0
        beq.s   4f
        lea     lcell_{HS[2]}(%pc),%a1
4:      move.l  %a1,{G('LCELL')}
| random model and state
        movea.l {P('U')},%a0
        move.w  {G('R')},%d6
        subq.w  #1,%d6
        jsr     fill_codes
        movea.l {P('WY')},%a0
        moveq   #{NOUT - 1},%d6
        jsr     fill_codes
        movea.l {P('M')},%a0
        move.l  {DOFF['nm']}(%a4),%d6
        subq.w  #1,%d6
        move.w  #4096,%d5
        move.w  #2048,%d4
        jsr     fill_u
        move.l  {DOFF['nmn']}(%a4),%d6
        beq.s   5f
        subq.w  #1,%d6
        movea.l {P('MN')},%a0
        move.w  #8192,%d5
        move.w  #-2048,%d4
        jsr     fill_u
5:      movea.l {P('MY')},%a0
        moveq   #{NOUT - 1},%d6
        move.w  #4096,%d5
        move.w  #1024,%d4
        jsr     fill_u
        movea.l {P('BY')},%a0
        moveq   #{NOUT - 1},%d6
        move.w  #4096,%d5
        move.w  #-2048,%d4
        jsr     fill_u
        movea.l {P('B0')},%a0
        move.w  {G('R')},%d6
        subq.w  #1,%d6
        move.w  #2048,%d5
        move.w  #-1024,%d4
        jsr     fill_u
        movea.l {P('P')},%a0
        move.w  {G('R')},%d6
        mulu.w  #3,%d6
        subq.w  #1,%d6
        move.w  #2048,%d5
        move.w  #-1024,%d4
        jsr     fill_u
        movea.l {P('WDX')},%a0
        move.w  {G('R')},%d6
        subq.w  #1,%d6
        move.w  #4096,%d5
        move.w  #-2048,%d4
        jsr     fill_u
        movea.l {P('WDY')},%a0
        move.w  {G('R')},%d6
        subq.w  #1,%d6
        move.w  #4096,%d5
        move.w  #-2048,%d4
        jsr     fill_u
        movea.l {P('HST')},%a0
        move.w  {G('H')},%d6
        subq.w  #1,%d6
        move.w  #24000,%d5
        move.w  #-12000,%d4
        jsr     fill_u
        movea.l {P('C')},%a0
        move.w  {G('H')},%d6
        subq.w  #1,%d6
        move.w  #24000,%d5
        move.w  #-12000,%d4
        jsr     fill_u
        movea.l {P('WE')},%a0
        move.w  {G('R')},%d6
        lsl.w   #5,%d6
        subq.w  #1,%d6
        move.w  #128,%d5
        move.w  #-64,%d4
        jsr     fill_ub
        movea.l {P('EMB')},%a0
        move.w  #{NCAT * NEMB - 1},%d6
        move.w  #128,%d5
        move.w  #-64,%d4
        jsr     fill_ub
        jsr     p_fold
        jsr     p_hq
        cmpi.l  #2,{DOFF['kind']}(%a4)
        bne.s   10f
        jsr     force_sat
10:     jsr     p_lut
| verification: one full step, then an {TRACE_N}-step sampled trace
        jsr     snap
        moveq   #0,%d7
        jsr     dumps
        jsr     step_full
        jsr     snap
        moveq   #1,%d7
        jsr     dumps
        move.l  {P('TRACE')},{G('TRP')}
        move.w  #{TRACE_N - 1},{G('TRN')}
6:      jsr     step_full
        movea.l {P('SAMP')},%a0
        movea.l {G('TRP')},%a1
        moveq   #7,%d0
7:      move.l  (%a0)+,(%a1)+
        dbf     %d0,7b
        move.l  %a1,{G('TRP')}
        subq.w  #1,{G('TRN')}
        bpl.s   6b
        jsr     snap
        moveq   #2,%d7
        jsr     dumps
| timings
        clr.w   {G('PI')}
8:      movea.l {G('DESC')},%a0
        move.w  {G('PI')},%d0
        lsl.w   #2,%d0
        move.l  {DOFF['reps']}(%a0,%d0.w),%d2
        beq.s   9f
        lea     tags(%pc),%a1
        move.l  0(%a1,%d0.w),%d1
        lea     parts(%pc),%a1
        move.w  {G('PI')},%d0
        add.w   %d0,%d0
        move.w  0(%a1,%d0.w),%d0
        lea     0(%a1,%d0.w),%a0
        jsr     timeit
9:      addq.w  #1,{G('PI')}
        cmpi.w  #{len(PARTS)},{G('PI')}
        bne.s   8b
        rts

| force_sat (P4 only): HQ = +30 (+15 at H = 384) everywhere and U's first row all +7, so that
| row's true sum is beyond int16 while each half fits: the dual-accumulator combine must clamp it.
force_sat:
        movea.l {P('HQ')},%a0
        move.w  {G('H')},%d6
        moveq   #30,%d0                   | each half (at most 256 columns) must still fit:
        cmpi.w  #384,%d6                  | 15 at H = 384, 30 below
        bne.s   4f
        moveq   #15,%d0
4:      subq.w  #1,%d6
1:      move.w  %d0,(%a0)+
        dbf     %d6,1b
        movea.l {P('U')},%a0
        movea.l {G('DESC')},%a4
        move.l  {DOFF['na3']}(%a4),%d5
        move.l  {DOFF['na2']}(%a4),%d7
        move.w  %d5,%d4
        beq.s   3f
        jsr     sseg
3:      move.w  %d7,%d4
        jsr     sseg
        movea.l {G('DESC')},%a4
        rts
sseg:   subq.w  #1,%d4
        move.w  #-32768+510,%d3          | (q*256 + 255)*2 - 32768: both weights +7
2:      move.w  %d3,(%a0)+
        add.w   #512,%d3
        dbf     %d4,2b
        rts

| dumps: d7 = phase. Descriptor: per phase a count, then (pointer index*4, size) pairs.
dumps:  movea.l {G('DESC')},%a3
        lea     {DOFF['dumps']}(%a3),%a3
        bra.s   2f
1:      move.l  (%a3)+,%d0
        lsl.l   #3,%d0
        adda.l  %d0,%a3
2:      dbf     %d7,1b
        move.l  (%a3)+,%d6
        subq.w  #1,%d6
3:      move.l  (%a3)+,%d0
        lea     {GPTR}(%a5),%a0
        move.l  0(%a0,%d0.w),%d2
        move.l  (%a3)+,%d3
        moveq   #0,%d4
        move.w  {G('DID')},%d4
        addq.w  #1,{G('DID')}
        move.l  %a3,-(%sp)
        move.w  %d6,-(%sp)
        jsr     adddump
        move.w  (%sp)+,%d6
        movea.l (%sp)+,%a3
        dbf     %d6,3b
        jmp     flush
""")

emit("tags:")
for p in PARTS:
    emit(f"        .long   {fourcc(p.ljust(4))}")
emit("parts:")
for p in PARTS:
    target = {"prim": "step_prime", "step": "step_full"}.get(p, f"p_{p}")
    emit(f"        .short  {target} - parts")

# descriptors
CFGINFO = []
emit("        .even\ncfg_list:")
emit(f"        .short  {len(RUN)}")
for i, cfg in enumerate(CONFIGS):
    if cfg in RUN:
        emit(f"        .short  desc_{i} - cfg_list + {2 * 0}")
# note: cfg_list entries are relative to cfg_list itself (lea cfg_list(pc); adda.w)
for i, cfg in enumerate(CONFIGS):
    cell, H, kind = cfg
    R = rows_of(cell, H)
    lay, ptr, size = config_layout(*cfg)
    reps = reps_for(*cfg)
    segs = [(128, H // 2 - 128), (0, 128)] if (kind == "P4" and H // 2 > 128) else [(0, H // 2)]
    na3, na2 = (segs[0][1], segs[1][1]) if len(segs) == 2 else (0, segs[0][1])
    bpg = {"W4": 6, "B4": 10, "P4": 6}[kind]
    plan = dump_plan(*cfg)
    info = {"id": i, "cell": cell, "H": H, "kind": kind, "R": R, "G": groups(kind, H), "A": 127,
            "layout": lay, "bytes": size, "footprint": footprint(*cfg), "reps": reps,
            "est_cycles": est_cycles(*cfg), "run": cfg in RUN, "dumps": []}
    did = i * 64
    for ph in plan:
        names = []
        for n, s in ph:
            names.append((did, n, s))
            did += 1
        info["dumps"].append(names)
    CFGINFO.append(info)
    if cfg not in RUN:
        continue
    vals = {"id": i, "kind": KINDS.index(kind), "cell": CELLS.index(cell), "H": H, "G": groups(kind, H),
            "R": R, "A": 127, "na3": na3, "na2": na2, "eback": groups(kind, H) * bpg, "hsel": HS.index(H),
            "nm": 2 * H if cell == "gru" else 4 * H, "nmn": 2 * H if cell == "gru" else 0}
    emit(f"desc_{i}:")
    emit("        .long   " + ", ".join(str(vals[f]) for f in DFIELDS))
    emit("        .long   " + ", ".join(str(reps[p]) for p in PARTS))
    emit("        .long   " + ", ".join(str(ptr[n]) for n in PTRS))
    for ph in plan:
        emit(f"        .long   {len(ph)}")
        emit("        .long   " + ", ".join(f"{4 * PIDX[n]}, {s}" for n, s in ph))

# tables last (only set_tables refers to them)
emit("set_tables:")
for name, gname in (("SIG", "SIGC"), ("TANH", "TANHC"), ("SECH", "SECHC"), ("EXPS", "EXPSC"),
                    ("EXPP", "EXPP"), ("RTAB", "RTAB"), ("COS", "COS")):
    emit(f"        lea     T_{name}+{2 * CENTER[name]}(%pc),%a0\n        move.l  %a0,{G(gname)}")
emit("        rts")
for name, t in TABLES.items():
    emit(f"T_{name}:")
    for j in range(0, len(t), 16):
        emit("        .short  " + ", ".join(str(v) for v in t[j:j + 16]))
emit("code_end:")

text = re.sub(r"(jsr\s+|jmp\s+)([A-Za-z_][A-Za-z0-9_]*)\s*$", r"\1\2(%pc)", "\n".join(asm), flags=re.M)
os.makedirs(OUT, exist_ok=True)
open(os.path.join(OUT, "bench.s"), "w").write(text + "\n")
json.dump({"configs": CFGINFO, "main_bytes": MAIN, "parts": PARTS, "trace_n": TRACE_N, "nout": NOUT,
           "nemb": NEMB, "ncat": NCAT, "hs": HS, "fits": FITS, "res_size": RES_SIZE},
          open(os.path.join(OUT, "layout.json"), "w"), indent=1)
tot = sum(sum(c["reps"][p] * c["est_cycles"][p] for p in PARTS) for c in CFGINFO if c["run"]) * SLOWDOWN / CLOCK
print(f"bench.s: {len(asm)} lines; main buffer {MAIN:,} bytes; est. timing {tot:.0f} s emulated")

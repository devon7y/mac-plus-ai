#!/usr/bin/env python3
"""The 68000 code Dream generates for each layer at load time (the spec for bench/jit.c).

One call of a layer's code handles one grid position. Registers on entry:
  a0 = input base for this position, a1 = output base, a2 = this layer's bias vector,
  a3 = the middle of its 64 KB table. d0-d7 are free; a0/a1 are restored on return.

pw (1x1 conv, ternary): outputs in chunks of up to 7, accumulated in d1..d7:
      moveq #0,dj                       (each accumulator)
      move.w (a0)+,d0                   (each input with a non-zero weight in the chunk;
                                         lea d(a0),a0 first when the next input is not adjacent)
      add.w d0,dj / sub.w d0,dj         (each non-zero weight)
      lea -n(a0),a0                     (back to the position base)
    then per output: RQ (below), stored with (a1)+ when adjacent, else lea d(a1),a1 first.
dw (depthwise 3x3, ternary): per channel
      moveq #0,d1 ; add.w/sub.w d(a0),d1 (each non-zero tap; the first +1 tap is move.w d(a0),d1)
      RQ, move.w ...,(a1)+
RQ for accumulator dj, shift k, mode m:
      asr.w #s,dj (or asl.w) for k, in steps of at most 8;  add.w (a2)+,dj
      addh: add.w (a1),dj ;  table modes: ext.l dj ; add.l dj,dj ; move.w 0(a3,dj.l),(a1)+ (a3 = middle
      of a 65536-entry table, so the full int16 value is clamped);  raw: move.w dj,(a1)+
"""
import subprocess
import tempfile
import os

import numpy as np


def s16(v):
    assert -32768 <= v <= 32767, v
    return v & 0xFFFF


class Emitter:
    def __init__(self):
        self.w = []          # 16-bit words
        self.asm = []        # equivalent assembly text (for the encoding check)

    def op(self, words, text):
        self.w.extend(words)
        self.asm.append(text)

    # ---- instructions
    def moveq0(self, n):
        self.op([0x7000 | n << 9], f"moveq #0,%d{n}")

    def load_d0_inc(self):
        self.op([0x3018], "move.w (%a0)+,%d0")

    def lea_a0(self, d):
        self.op([0x41E8, s16(d)], f"lea {d}(%a0),%a0")

    def lea_a1(self, d):
        self.op([0x43E9, s16(d)], f"lea {d}(%a1),%a1")

    def addsub_d0(self, n, sign):
        self.op([(0xD040 if sign > 0 else 0x9040) | n << 9], f"{'add' if sign > 0 else 'sub'}.w %d0,%d{n}")

    def addsub_d7(self, n, sign):
        self.op([(0xD047 if sign > 0 else 0x9047) | n << 9], f"{'add' if sign > 0 else 'sub'}.w %d7,%d{n}")

    def twice_d0_to_d7(self):
        self.op([0x3E00, 0xDE47], "move.w %d0,%d7\n        add.w %d7,%d7")

    def move_a0d(self, d, n):
        if d == 0:
            self.op([0x3010 | n << 9], f"move.w (%a0),%d{n}")
        else:
            self.op([0x3028 | n << 9, s16(d)], f"move.w {d}(%a0),%d{n}")

    def addsub_a0d(self, d, n, sign):
        op = "add" if sign > 0 else "sub"
        if d == 0:
            self.op([(0xD050 if sign > 0 else 0x9050) | n << 9], f"{op}.w (%a0),%d{n}")
        else:
            self.op([(0xD068 if sign > 0 else 0x9068) | n << 9, s16(d)], f"{op}.w {d}(%a0),%d{n}")

    def shift(self, n, k):
        while k > 0:
            s = min(k, 8)
            self.op([0xE040 | (s & 7) << 9 | n], f"asr.w #{s},%d{n}")
            k -= s
        while k < 0:
            s = min(-k, 8)
            self.op([0xE140 | (s & 7) << 9 | n], f"asl.w #{s},%d{n}")
            k += s

    def add_bias(self, n):
        self.op([0xD05A | n << 9], f"add.w (%a2)+,%d{n}")

    def add_old(self, n):
        self.op([0xD051 | n << 9], f"add.w (%a1),%d{n}")

    def double(self, n):
        self.op([0x48C0 | n, 0xD080 | n << 9 | n], f"ext.l %d{n}\n        add.l %d{n},%d{n}")

    def store_tab(self, n):
        self.op([0x32F3, n << 12 | 0x0800], f"move.w 0(%a3,%d{n}.l),(%a1)+")

    def store_raw(self, n):
        self.op([0x32C0 | n], f"move.w %d{n},(%a1)+")

    def sext15(self, n):
        self.op([0xD040 | n << 9 | n, 0xE240 | n], f"add.w %d{n},%d{n}\n        asr.w #1,%d{n}")

    def clamp_store(self, n, lo, hi):
        """cmp/branch clamp to [lo, hi], then move.w dn,(a1)+ (no table: the 1 MB build)."""
        self.op([0x0C40 | n, s16(lo), 0x6C06, 0x303C | n << 9, s16(lo), 0x600A,
                 0x0C40 | n, s16(hi), 0x6F04, 0x303C | n << 9, s16(hi), 0x32C0 | n],
                f"cmp.w #{lo},%d{n}\n        bge.s 1f\n        move.w #{lo},%d{n}\n        bra.s 2f\n"
                f"1:      cmp.w #{hi},%d{n}\n        ble.s 2f\n        move.w #{hi},%d{n}\n"
                f"2:      move.w %d{n},(%a1)+")

    def rts(self):
        self.op([0x4E75], "rts")


def rq(e, n, k, mode, clamp=None):
    """k: an int shift, or (k1, e2): two-term gain (acc >> k1) + s (acc >> k2), via d0:
    move.w dn,d0; shift d0 by k2; shift dn by k1; add.w/sub.w d0,dn."""
    if np.ndim(k) == 1:
        k1, e2 = int(k[0]), int(k[1])
        if e2:
            e.op([0x3000 | n], f"move.w %d{n},%d0")
            e.shift(0, abs(e2) - 4)
            e.shift(n, k1)
            e.addsub_d0(n, 1 if e2 > 0 else -1)
        else:
            e.shift(n, k1)
    else:
        e.shift(n, int(k))
    e.add_bias(n)
    if clamp is not None:                    # table-free form (1 MB build): sign-extend 15 bits, clamp
        if mode == "addh":
            e.add_old(n)
        e.clamp_store(n, *clamp)
        return
    if mode == "addh":
        e.add_old(n)
    if mode == "raw":
        e.store_raw(n)
    else:
        e.double(n)
        e.store_tab(n)


def pw_offsets(l, T):
    """Byte offsets of the inputs (relative to a0) and outputs (relative to a1)."""
    src, dst = T[l["src"]], T[l["dst"]]
    Hs, Ws, Cs = src
    Hd, Wd, Cd = dst
    if l["gather"] == "s2d":
        offin = [((dy * (Ws + 2) + dx) * Cs + c) * 2 for dy in (0, 1) for dx in (0, 1) for c in range(Cs)]
    else:
        offin = [c * 2 for c in range(Cs)]
    if l["scatter"] == "d2s":
        offout = [((dy * (Wd + 2) + dx) * Cd + c) * 2 for dy in (0, 1) for dx in (0, 1) for c in range(Cd)]
    else:
        offout = [c * 2 for c in range(Cd)]
    assert len(offin) == l["cin"] and len(offout) == l["cout"]
    return offin, offout


def emit_pw(l, T, w, k, clamp=None):
    """Ternary weights; with l["wq"] == "q4", 4-bit weights (-7..7): 6 accumulators d1..d6,
    d7 = 2*x, and |w| = 2a + b becomes a adds of d7 and b adds of d0."""
    e = Emitter()
    offin, offout = pw_offsets(l, T)
    cin, cout = l["cin"], l["cout"]
    q4 = l.get("wq") == "q4"
    csz = 6 if q4 else 7
    ocur = 0
    for o0 in range(0, cout, csz):
        m = min(csz, cout - o0)
        for j in range(m):
            e.moveq0(j + 1)
        cur = 0
        for i in range(cin):
            col = w[o0:o0 + m, i]
            if not col.any():
                continue
            if offin[i] != cur:
                e.lea_a0(offin[i] - cur)
                cur = offin[i]
            e.load_d0_inc()
            cur += 2
            if q4 and (abs(col) >= 2).any():
                e.twice_d0_to_d7()
            for j in range(m):
                v = int(col[j])
                if q4:
                    for _ in range(abs(v) // 2):
                        e.addsub_d7(j + 1, v)
                    if abs(v) & 1:
                        e.addsub_d0(j + 1, v)
                elif v:
                    e.addsub_d0(j + 1, v)
        if cur:
            e.lea_a0(-cur)
        for j in range(m):
            o = o0 + j
            if offout[o] != ocur:
                e.lea_a1(offout[o] - ocur)
                ocur = offout[o]
            rq(e, j + 1, k[o], l["mode"], None if l["mode"] == "raw" else clamp)
            ocur += 2
    if ocur:
        e.lea_a1(-ocur)
    e.rts()
    return e


def emit_dw(l, T, w, k, clamp=None):
    """clamp=(lo, hi): the table-free requantisation of the 1 MB build."""
    e = Emitter()
    H, W, C = T[l["src"]]
    taps = [(0, 0)] if l["center"] else [(dy, dx) for dy in (-1, 0, 1) for dx in (-1, 0, 1)]
    for c in range(C):
        first = True
        nz = [(t, int(w[c, t])) for t in range(len(taps)) if w[c, t]]
        if not nz or nz[0][1] < 0:
            e.moveq0(1)
            first = False
        for t, s in nz:
            dy, dx = taps[t]
            d = ((dy * (W + 2) + dx) * C + c) * 2
            if first:
                e.move_a0d(d, 1)
                first = False
            else:
                e.addsub_a0d(d, 1, s)
        rq(e, 1, k[c], l["mode"], clamp)
    e.lea_a1(-2 * C)
    e.rts()
    return e


def emit(l, T, w, k):
    return (emit_pw if l["kind"] == "pw" else emit_dw)(l, T, w, k)


def check_encoding(e):
    """Assemble e.asm with m68k-elf-as and compare with e.w."""
    with tempfile.TemporaryDirectory() as d:
        s, o, b = (os.path.join(d, n) for n in ("t.s", "t.o", "t.bin"))
        open(s, "w").write(".text\n" + "\n".join("        " + a for a in e.asm) + "\n")
        subprocess.run(["m68k-elf-as", "-m68000", "-o", o, s], check=True)
        subprocess.run(["m68k-elf-objcopy", "-O", "binary", "-j", ".text", o, b], check=True)
        got = open(b, "rb").read()
    want = b"".join(x.to_bytes(2, "big") for x in e.w)
    return got == want, got, want


if __name__ == "__main__":
    import numpy as np
    import arch
    import ref
    T2, L2 = arch.build("A64L2Q")
    bad2 = 0
    for i, l in enumerate(L2[:12] + L2[-3:]):
        w, k, b = ref.random_layer(i, l)
        ok, _, _ = check_encoding(emit(l, T2, w, k))
        bad2 += not ok
    print(f"A64L2Q (two-term gains, 4-bit in/out): {'OK' if not bad2 else 'MISMATCH'}")
    T, L = arch.build("A64S")
    l0 = [l for l in L if l["kind"] == "dw" and not l["center"]][0]
    w0, k0, _ = ref.random_layer(1, l0)
    ok, _, _ = check_encoding(emit_dw(l0, T, w0, k0, clamp=(-127, 127)))
    print(f"dw with clamps: {'OK' if ok else 'MISMATCH'}")
    for l in L:
        if l["kind"] == "pw" and l["tag"].endswith(".pe"):
            l["wq"] = "q4"
    bad = 0
    for i, l in enumerate(L):
        w, k, b = ref.random_layer(i, l)
        if l.get("wq") == "q4":
            w = (np.random.RandomState(i).randint(-7, 8, size=w.shape)).astype(np.int8)
        k = k.copy()
        k[::3] = -2           # exercise asl too
        k[1::5] = 11          # and two-step shifts
        e = emit(l, T, w, k)
        ok, got, want = check_encoding(e)
        bad += not ok
        print(f"{l['tag']:10s} {len(e.w) * 2:7d} bytes  {'OK' if ok else 'MISMATCH'}")
        if not ok:
            for j in range(0, min(len(got), len(want)), 2):
                if got[j:j + 2] != want[j:j + 2]:
                    print("   first diff at byte", j, got[j:j + 8].hex(), want[j:j + 8].hex())
                    break
    print("ALL ENCODINGS MATCH" if not bad else f"{bad} MISMATCHES")

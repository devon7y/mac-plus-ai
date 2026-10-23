#!/usr/bin/env python3
"""Check the benchmark's kernels against a Python reference, using the test buffers
the 68000 program left in memory (dumped by bench.lua).

Usage: verify.py <layout.json> <dump dir>
"""
import json
import os
import struct
import sys

lay = json.load(open(sys.argv[1]))
ddir = sys.argv[2]
dumps = {name: open(os.path.join(ddir, f"dump_{i}.bin"), "rb").read() for i, name in enumerate(lay["dumps"])}
TG, TR, HD = lay["TG"], lay["TR"], lay["HD"]


def words(b):
    return list(struct.unpack(f">{len(b) // 2}h", b))


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


ok = True
for v, (k, kind) in lay["variants"].items():
    x = words(dumps[f"T{v}_X"])
    out = words(dumps[f"T{v}_O"])
    raw = dumps[f"T{v}_W"]
    codes = words(raw) if kind == "W" else list(raw)
    exp = []
    for r in range(TR):
        acc = 0
        for g in range(TG):
            c = codes[r * TG + g]
            if kind == "W":
                idx = (((c + 32768) // 2) - g) // TG
            else:
                idx = c // 2 if k == 4 else c
            for j in range(k):
                acc += ((idx // 3 ** j) % 3 - 1) * x[g * k + j]
        exp.append(s16(acc))
    good = exp == out
    ok &= good
    print(f"ternary {v}: {'OK' if good else 'MISMATCH'}  got {out}  expected {exp}")

h = words(dumps["TP_H"])
codes = words(dumps["TP_W"])
out = words(dumps["TP_O"])
exp = []
for r in range(3):
    acc = 0
    for p in range(4):
        c = (codes[r * 4 + p] + 32768) // 2 - p * 256
        acc += (c // 16 - 8) * h[2 * p] + (c % 16 - 8) * h[2 * p + 1]
    exp.append(s16(acc))
good = exp == out
ok &= good
print(f"4-bit pair kernel: {'OK' if good else 'MISMATCH'}  got {out}  expected {exp}")

e, expt = 65280, []
for _ in range(256):
    expt.append(e >> 8)
    e = (e * 61566) >> 16
good = expt == [w & 0xFFFF for w in words(dumps["EXPT"])]
ok &= good
print(f"exp table: {'OK' if good else 'MISMATCH'}  first values {expt[:6]}")

sc = words(dumps["TS_SC"])
pt = [w & 0xFFFF for w in words(dumps["TS_PT"])]
vc = list(dumps["TS_VC"])
acc_out = words(dumps["TS_ACC"])
mx = max(sc)
ptab, exp_acc = [], [0] * HD
for t, s in enumerate(sc):
    dd = ((mx - s) & 0xFFFF) >> 2
    p = expt[min(dd, 255)]
    ptab += [(-8 * p + i * p) & 0xFFFF for i in range(16)]
    for dim in range(HD):
        exp_acc[dim] += (vc[t * HD + dim] // 2 - 8) * p
good_pt = ptab == pt
good_acc = [s16(a) for a in exp_acc] == acc_out
ok &= good_pt and good_acc
print(f"softmax tables: {'OK' if good_pt else 'MISMATCH'}; weighted sum: {'OK' if good_acc else 'MISMATCH'}"
      f"  (first dims got {acc_out[:4]}, expected {[s16(a) for a in exp_acc[:4]]})")
print("ALL KERNELS CORRECT" if ok else "SOME KERNELS WRONG")
sys.exit(0 if ok else 1)

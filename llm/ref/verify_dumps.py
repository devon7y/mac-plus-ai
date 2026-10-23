#!/usr/bin/env python3
"""Reproduce the 68000 benchmark's dumped test buffers with the reference kernels (kernels.py).

Usage: verify_dumps.py <layout.json> <dump dir>
For each test the 68000 left its inputs and outputs in memory; we rebuild the lookup tables from
the dumped inputs with the emulated table builders, run the emulated lookup kernels on the dumped
codes, and require every output word to match.
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import kernels as K  # noqa: E402


def words(b):
    return list(struct.unpack(f">{len(b) // 2}h", b))


def check(layout_path, ddir, quiet=False):
    lay = json.load(open(layout_path))
    dumps = {n: open(os.path.join(ddir, f"dump_{i}.bin"), "rb").read() for i, n in enumerate(lay["dumps"])}
    TG, TR, HD = lay["TG"], lay["TR"], lay["HD"]
    results = []
    for v, (k, kind) in lay["variants"].items():
        x = words(dumps[f"T{v}_X"])
        out = words(dumps[f"T{v}_O"])
        raw = dumps[f"T{v}_W"]
        tables = K.build_tables(x, k, TG)
        if kind == "W":
            codes = [words(raw)[r * TG:(r + 1) * TG] for r in range(TR)]
            got = K.mv_W(codes, K.lut_interleaved(tables), TG)
        else:
            codes = [list(raw[r * TG:(r + 1) * TG]) for r in range(TR)]
            mv = K.mv_B4 if k == 4 else K.mv_B5
            got = mv(codes, K.lut_contiguous(tables))
        results.append((f"ternary {v} ({TR} rows x {TG} groups)", got == out, got, out))
    h = words(dumps["TP_H"])
    codes = words(dumps["TP_W"])
    out = words(dumps["TP_O"])
    lut = K.build_pair_tables(h)
    got = K.mv_W([codes[r * 4:(r + 1) * 4] for r in range(3)], lut, None)
    results.append(("4-bit pair tables + lookup", got == out, got, out))
    expt = [w & 0xFFFF for w in words(dumps["EXPT"])]
    results.append(("exp table (256 entries)", expt == K.EXPT, expt[:6], K.EXPT[:6]))
    sc = words(dumps["TS_SC"])
    pt = words(dumps["TS_PT"])
    ptab = K.softmax_bench(sc)
    results.append(("softmax -> per-position tables", ptab == pt, ptab[:4], pt[:4]))
    vc = list(dumps["TS_VC"])
    acc = words(dumps["TS_ACC"])
    got = K.wsum(vc, pt, HD)
    results.append(("weighted sum (5 positions x 40 dims)", got == acc, got[:4], acc[:4]))
    ok = all(r[1] for r in results)
    if not quiet:
        for name, good, a, b in results:
            print(f"{'OK      ' if good else 'MISMATCH'} {name}: ref {a} 68000 {b}")
        print("ALL KERNELS BIT-EXACT" if ok else "SOME KERNELS DIFFER")
    return ok


if __name__ == "__main__":
    sys.exit(0 if check(sys.argv[1], sys.argv[2]) else 1)

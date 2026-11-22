#!/usr/bin/env python3
"""Bit-exactness of app/recog.c + the generated classifier (host build) against calc/ref/decode.py
and ref.py, on whole handwritten lines from held-out writers (exprset.py), with random user
corrections on some lines.

    python3 -I calc/app/tests/test_recog.py <model.npz> <gen dir> [--n 300]
"""
import argparse
import os
import struct
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
CALC = os.path.dirname(APP)
sys.path.insert(0, os.path.join(CALC, "ref"))
import ref                                                    # noqa: E402
from decode import segment, viterbi, group_raster, raised_flags   # noqa: E402
from exprset import make_line, pools, random_expr             # noqa: E402
from synth import Glyphs                                      # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("gen")
    ap.add_argument("--n", type=int, default=300)
    a = ap.parse_args()
    exe = os.path.join(a.gen, "host_recog")
    subprocess.run(["clang", "-O1", "-Wall", "-DNET_HOST", "-I", a.gen, "-I", APP, "-o", exe,
                    os.path.join(HERE, "host_recog.c"), os.path.join(APP, "recog.c"),
                    os.path.join(a.gen, "net_gen.c")], check=True)
    M = ref.load(a.model)
    P = pools()
    rng = np.random.default_rng(123)
    g = Glyphs(np.random.default_rng(456))
    lines, buf = [], bytearray()
    for it in range(a.n):
        labels = random_expr(rng) + ["="]
        if it % 4 == 1:                        # some lines with exponents (written raised)
            labels = [str(rng.integers(1, 10))] + ["^" + str(rng.integers(0, 10)) for _ in range(rng.integers(1, 3))] \
                + ["+", str(rng.integers(1, 10)), "="]
        strokes = make_line(labels, P, g, rng)
        if it % 10 == 0:                       # some lines with a few tiny/dot strokes added
            strokes.append(np.array([[strokes[-1][0, 0] + 30, strokes[-1][0, 1]]]))
        groups, href = segment(strokes)
        forced = {}
        if it % 3 == 0 and groups:
            for _ in range(rng.integers(1, 3)):
                forced[int(rng.integers(len(groups)))] = int(rng.integers(18))
        lines.append((strokes, forced))
        buf += struct.pack("<h", len(strokes))
        for s in strokes:
            buf += struct.pack("<h", len(s))
            buf += np.asarray(s, "<i2").tobytes()
        buf += struct.pack("<h", len(forced))
        for k, v in forced.items():
            buf += struct.pack("<hh", k, v)
    out = subprocess.run([exe], input=bytes(buf), capture_output=True, check=True).stdout
    pos = 0

    def rd():
        nonlocal pos
        v = struct.unpack_from("<h", out, pos)[0]
        pos += 2
        return v
    bad = 0
    nsym = 0
    nraised = 0
    for strokes, forced in lines:
        groups, href = segment(strokes)
        imgs = np.stack([group_raster(strokes, gr, href) for gr in groups])
        S = ref.forward_batch(M, imgs)
        dec, ra, _ = viterbi(S, forced, raised_flags(strokes, groups, href))
        c_href, c_ng = rd(), rd()
        ok = c_href == href and c_ng == len(groups)
        for gi in range(c_ng):
            n = rd()
            stk = [rd() for _ in range(n)]
            sc = [rd() for _ in range(18)]
            tk = rd()
            if gi < len(groups):
                ok = ok and stk == groups[gi] and sc == [int(v) for v in S[gi]] and tk == dec[gi] + 32 * ra[gi]
        nsym += len(groups)
        nraised += sum(ra)
        bad += not ok
    print(f"{len(lines)} lines, {nsym} symbols ({nraised} read as superscripts): {bad} lines differ")
    print("RESULT", "PASS" if bad == 0 else "FAIL")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

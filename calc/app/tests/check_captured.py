#!/usr/bin/env python3
"""Read the strokes the Mac captured in a MAME run (m2.lua's captured.txt) with the Python reference,
and compare with the strokes that were sent (make_mouse_test.py's input).

    python3 -I calc/app/tests/check_captured.py <run dir>/captured.txt calc/build/test/m2_input.txt
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CALC = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(CALC, "ref"))
import ref                                              # noqa: E402
from decode import segment, viterbi, group_raster, raised_flags   # noqa: E402
from strokes import CLASSES                             # noqa: E402


def parse(path):
    L = []
    for line in open(path):
        if line.startswith("E "):
            L.append((line[2:].strip(), []))
        elif line.startswith("S "):
            v = [int(t) for t in line[2:].split()]
            L[-1][1].append(np.array(v).reshape(-1, 2))
    return L


def read(M, st):
    g, href = segment(st)
    if not g:
        return "", href
    S = ref.forward_batch(M, np.stack([group_raster(st, gr, href) for gr in g]))
    d, ra, _ = viterbi(S, None, raised_flags(st, g, href))
    return "".join(("^" if r else "") + CLASSES[c] for c, r in zip(d, ra)), href


def main():
    M = ref.load(os.path.join(CALC, "model", "scribble_model.npz"))
    cap, inp = parse(sys.argv[1]), parse(sys.argv[2])
    nsym = ok = 0
    for (t, cs), (_, ins) in zip(cap, inp):
        rc, hc = read(M, cs)
        ri, hi = read(M, ins)
        tl = t.replace("^", "")
        rl = rc.replace("^", "")
        nsym += len(tl)
        if len(rl) == len(tl):
            ok += sum(a == b for a, b in zip(rl, tl))
        print(f"{t:14s} captured {len(cs):2d} strokes {sum(len(s) for s in cs):4d} pts -> {rc:14s} (href {hc}) | "
              f"sent {len(ins):2d} strokes {sum(len(s) for s in ins):4d} pts -> {ri:14s} (href {hi})")
    print(f"symbols read right (lines segmented right): {ok} of {nsym}")


if __name__ == "__main__":
    main()

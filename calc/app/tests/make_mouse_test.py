#!/usr/bin/env python3
"""Test input for the MAME check (tests/m2.lua): classifier test vectors and handwritten lines.

    python3 -I calc/app/tests/make_mouse_test.py [--out calc/build/test/m2_input.txt] [--paper-w 432]

Lines of the output:
  R <72 raster bytes, hex> <18 expected scores>     a raster for the mailbox (bit-exact check)
  E <truth labels>                                    a new line to write (labels as in strokes.CLASSES)
  S x y x y ...                                       one stroke in paper coordinates
The strokes are whole expressions from held-out writers (UJI tst, pendigits tes) and synthetic
operators, made mouse-like by exprset.make_line, scaled to fit the paper.
"""
import argparse
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CALC = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(CALC, "ref"))
import ref                                              # noqa: E402
from exprset import make_line, pools                    # noqa: E402
from synth import Glyphs                                # noqa: E402

LINES = ["12x(3.5+4)=", "7/0=", "250-17.5=", "(8+2)x(3-1)=", "1/3=", "-4.2x5=", "64+36=", "9x9-1=",
         "2^3+1=", "(1+2)^2="]          # '^' = the next symbol is written as a superscript


def labels_of(text):
    out, i = [], 0
    while i < len(text):
        if text[i] == "^":
            out.append("^" + text[i + 1])
            i += 2
        else:
            out.append(text[i])
            i += 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(CALC, "build", "test", "m2_input.txt"))
    ap.add_argument("--paper-w", type=int, default=432)
    ap.add_argument("--seed", type=int, default=2026)
    a = ap.parse_args()
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    M = ref.load(os.path.join(CALC, "model", "scribble_model.npz"))
    out = []
    d = np.load(os.path.join(CALC, "build", "data", "test_uji_mouse.npz"))
    rs = np.random.default_rng(5).choice(len(d["X"]), 8, replace=False)
    for i in rs:
        img = np.unpackbits(d["X"][i]).reshape(24, 24)
        sc = ref.forward(M, img)
        out.append("R " + d["X"][i].tobytes().hex() + " " + " ".join(str(int(v)) for v in sc))
    P = pools()
    rng = np.random.default_rng(a.seed)
    g = Glyphs(np.random.default_rng(a.seed + 1))
    for text in LINES:
        labels = labels_of(text)
        strokes = make_line(labels, P, g, rng)
        pts = np.concatenate(strokes)
        x0, y0 = pts.min(0)
        x1, y1 = pts.max(0)
        # fit: about 50 px tall digits, leaving room for the answer; keep inside the 140 px paper
        f = min(1.0, (a.paper_w * 0.62) / max(1, x1 - x0), 100.0 / max(1, y1 - y0))
        out.append("E " + text)
        for s in strokes:
            q = np.round((s - [x0, y0]) * f + [12, 22]).astype(int)
            keep = np.ones(len(q), bool)
            keep[1:] = (np.diff(q, axis=0) != 0).any(1)
            q = q[keep]
            out.append("S " + " ".join(f"{x} {y}" for x, y in q))
    open(a.out, "w").write("\n".join(out) + "\n")
    print("wrote", a.out, len(out), "lines")


if __name__ == "__main__":
    main()

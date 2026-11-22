#!/usr/bin/env python3
"""Superscript detection: false alarms on lines without powers, and lines with powers."""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from decode import segment, raised_flags, viterbi, group_raster   # noqa: E402
from exprset import make_line, pools, random_expr                  # noqa: E402
from synth import Glyphs                                           # noqa: E402
from strokes import CLASSES                                        # noqa: E402
import ref                                                         # noqa: E402

M = ref.load(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "model", "scribble_model.npz"))
P = pools()
rng = np.random.default_rng(11)
g = Glyphs(np.random.default_rng(12))
fa = groups_total = 0
lines_fa = 0
N = 300
for it in range(N):
    labels = random_expr(rng) + ["="]
    strokes = make_line(labels, P, g, rng)
    groups, href = segment(strokes)
    r = raised_flags(strokes, groups, href)
    imgs = np.stack([group_raster(strokes, gr, href) for gr in groups])
    d, rr, _ = viterbi(ref.forward_batch(M, imgs), None, r)
    fa += sum(rr)
    lines_fa += any(rr)
    groups_total += len(groups)
print(f"no-power lines: {fa} of {groups_total} symbols flagged raised; {lines_fa} of {N} lines affected")
ok = okdec = 0
N2 = 200
for it in range(N2):
    base = [str(rng.integers(1, 10))]
    ex = ["^" + str(rng.integers(0, 10)) for _ in range(rng.choice([1, 1, 2]))]
    labels = (["("] + random_expr(rng) + [")"] if rng.random() < 0.2 else []) or base
    labels = labels + ex
    if rng.random() < 0.5:
        labels = labels + [str(rng.choice(["+", "-", "x"]))] + [str(rng.integers(1, 100))[0]]
    labels = labels + ["="]
    strokes = make_line(labels, P, g, rng)
    groups, href = segment(strokes)
    r = raised_flags(strokes, groups, href)
    truth_r = [l.startswith("^") for l in labels]
    if len(groups) == len(labels):
        imgs0 = np.stack([group_raster(strokes, gr, href) for gr in groups])
        ok += viterbi(ref.forward_batch(M, imgs0), None, r)[1] == truth_r
        imgs = np.stack([group_raster(strokes, gr, href) for gr in groups])
        S = ref.forward_batch(M, imgs)
        d, rr, _ = viterbi(S, None, r)
        okdec += rr == truth_r and [CLASSES[c] for c in d] == [l.lstrip("^") for l in labels]
print(f"power lines: raised flags exactly right in {ok} of {N2}; whole line right {okdec} of {N2}")

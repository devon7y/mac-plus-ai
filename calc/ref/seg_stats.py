#!/usr/bin/env python3
"""Segmentation error statistics on exprset lines: which symbols get split or merged."""
import os
import sys
from collections import Counter

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from decode import segment                             # noqa: E402
from exprset import make_line, pools, random_expr      # noqa: E402
from synth import Glyphs                               # noqa: E402

P = pools()
rng = np.random.default_rng(7)
g = Glyphs(np.random.default_rng(1007))
split, merged = Counter(), Counter()
nsym = Counter()
lines_ok = 0
N = int(sys.argv[1]) if len(sys.argv) > 1 else 400
for it in range(N):
    labels = random_expr(rng) + ["="]
    owner = []
    strokes = make_line(labels, P, g, rng, owner)
    groups, href = segment(strokes)
    truth = {}
    for k, o in enumerate(owner):
        truth.setdefault(o, set()).add(k)
    gsets = [set(gr) for gr in groups]
    ok = True
    for o, ss in truth.items():
        nsym[labels[o]] += 1
        if ss in gsets:
            continue
        ok = False
        owners_in = [gs for gs in gsets if gs & ss]
        if len(owners_in) > 1:
            split[labels[o]] += 1
        else:
            others = sorted({owner[k] for k in owners_in[0]} - {o})
            merged["".join(labels[x] for x in sorted(others + [o]))] += 1
    lines_ok += ok
print(f"lines ok {lines_ok}/{N}")
print("split:", ", ".join(f"{k} {v}/{nsym[k]}" for k, v in split.most_common()))
print("merged:", ", ".join(f"{k}:{v}" for k, v in merged.most_common(25)))

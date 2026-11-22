#!/usr/bin/env python3
"""Render failing segmentation cases of exprset to PNGs (calc/build/debug/seg_*.png)."""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from decode import segment                             # noqa: E402
from exprset import make_line, pools, random_expr      # noqa: E402
from synth import Glyphs                               # noqa: E402
from strokes import CALC                               # noqa: E402

P = pools()
rng = np.random.default_rng(int(sys.argv[1]) if len(sys.argv) > 1 else 7)
g = Glyphs(np.random.default_rng(1007))
out = os.path.join(CALC, "build", "debug")
nshow = 0
for it in range(400):
    labels = random_expr(rng) + ["="]
    strokes = make_line(labels, P, g, rng)
    groups, href = segment(strokes)
    if len(groups) == len(labels):
        continue
    pts = np.concatenate(strokes)
    W, H = int(pts[:, 0].max()) + 30, int(pts[:, 1].max()) + 30
    im = Image.new("RGB", (W, H), "white")
    d = ImageDraw.Draw(im)
    cols = [(220, 0, 0), (0, 140, 0), (0, 0, 220), (200, 120, 0), (150, 0, 150)]
    for gi, gr in enumerate(groups):
        c = cols[gi % len(cols)]
        for k in gr:
            s = strokes[k]
            if len(s) == 1:
                d.rectangle([s[0, 0] - 1, s[0, 1] - 1, s[0, 0] + 1, s[0, 1] + 1], fill=c)
            else:
                d.line([tuple(p) for p in s], fill=c, width=2)
    im.save(os.path.join(out, f"seg_{nshow}.png"))
    print(nshow, "".join(labels), "groups", len(groups), "truth", len(labels), "href", href)
    nshow += 1
    if nshow >= 12:
        break

#!/usr/bin/env python3
"""Art-review helper: a rough in-game mock (gravel track, rails) with sprites placed by anchor.

    python3 mock.py out.png name:x:y [name:x:y ...]     (x, y = anchor position on a 384x342 field)
Writes the field at 2x (about the physical size of a Mac Plus screen on a modern display).
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import sprites as S  # noqa: E402

W, H = 384, 342
sp = S.load()
bg = S.gravel(H, W)
bg[:120, :] = False                      # sky / horizon area white
for i in range(120, H):                  # converging rails
    t = (i - 120) / (H - 120)
    for lane in (-1.5, -0.5, 0.5, 1.5):
        for d in (-0.18, 0.18):
            x = int(W / 2 + (lane + d * 0) * (40 + 220 * t) + d * (40 + 220 * t))
            if 0 <= x < W:
                bg[i, x] = True
for arg in sys.argv[2:]:
    n, x, y = arg.split(":")
    s = sp[n]
    S.composite(bg, s, int(x) - s.ax, int(y) - s.ay)
S.to_image(bg, 2).save(sys.argv[1])
print(sys.argv[1])

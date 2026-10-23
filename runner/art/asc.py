#!/usr/bin/env python3
"""Art-review helper: render ASCII blocks from a scratch text file (same format as sprites.txt).

    python3 asc.py in.txt out.png [zoom]
Shows each block at 2x ("real size" on a modern display) and at zoom, on white and on gravel.
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import sprites as S  # noqa: E402

src, out = sys.argv[1], sys.argv[2]
zoom = int(sys.argv[3]) if len(sys.argv) > 3 else 6
sp = S.load(src)
tiles = []
for n, s in sp.items():
    cw, ch = s.w + 6, s.h + 6
    a = np.zeros((ch, cw), bool)
    S.composite(a, s, 3, 3)
    b = S.gravel(ch, cw)
    S.composite(b, s, 3, 3)
    tiles.append((n, a, b))
W = 1000
pad = 6
x = y = rowh = 0
placed = []
for n, a, b in tiles:
    tw = a.shape[1] * 2 * 2 + a.shape[1] * zoom * 2 + pad * 4
    th = a.shape[0] * zoom + 14
    if x + tw > W and x:
        x, y, rowh = 0, y + rowh + pad, 0
    placed.append((n, a, b, x, y))
    x += tw + pad * 2
    rowh = max(rowh, th)
Wt = max(p[3] + p[1].shape[1] * 4 + p[1].shape[1] * zoom * 2 + pad * 4 for p in placed)
img = Image.new("L", (min(W, Wt + pad), y + rowh + pad), 170)
d = ImageDraw.Draw(img)
for n, a, b, x, y in placed:
    d.text((x, y), n, fill=0)
    yy = y + 12
    img.paste(S.to_image(a, 2), (x, yy))
    img.paste(S.to_image(b, 2), (x, yy + a.shape[0] * 2 + 2))
    xx = x + a.shape[1] * 2 + pad
    img.paste(S.to_image(a, zoom), (xx, yy))
    img.paste(S.to_image(b, zoom), (xx + a.shape[1] * zoom + pad, yy))
img.save(out)
print(out, img.size)

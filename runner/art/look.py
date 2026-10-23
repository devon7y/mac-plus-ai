#!/usr/bin/env python3
"""Art-review helper: render chosen sprites from sprites.txt at 1x and a zoom, on white and gravel.

    python3 look.py out.png [zoom] name [name ...]     (name may be a prefix ending in '*')

Each sprite gets four panels: 1x white, 1x gravel, zoom white, zoom gravel.
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import sprites as S  # noqa: E402


def main():
    out = sys.argv[1]
    zoom = 4
    args = sys.argv[2:]
    if args and args[0].isdigit():
        zoom, args = int(args[0]), args[1:]
    sp = S.load()
    names = []
    for a in args:
        if a.endswith("*"):
            names += [n for n in sp if n.startswith(a[:-1])]
        else:
            names.append(a)
    tiles = []
    for n in names:
        s = sp[n]
        cw, ch = s.w + 6, s.h + 6
        a = np.zeros((ch, cw), bool)
        S.composite(a, s, 3, 3)
        b = S.gravel(ch, cw)
        S.composite(b, s, 3, 3)
        tiles.append((n, a, b))
    pad = 8
    W = 1000
    x = y = 0
    rowh = 0
    placed = []
    for n, a, b in tiles:
        tw = a.shape[1] * 2 + a.shape[1] * zoom * 2 + pad * 3
        th = a.shape[0] * zoom + 14
        if x + tw > W and x:
            x, y, rowh = 0, y + rowh + pad, 0
        placed.append((n, a, b, x, y))
        x += tw + pad * 2
        rowh = max(rowh, th)
    Wt = max(p[3] + p[1].shape[1] * 2 + p[1].shape[1] * zoom * 2 + pad * 3 for p in placed)
    img = Image.new("L", (min(W, Wt + pad), y + rowh + pad), 170)
    d = ImageDraw.Draw(img)
    for n, a, b, x, y in placed:
        d.text((x, y), f"{n} {a.shape[1]-6}x{a.shape[0]-6}", fill=0)
        yy = y + 12
        img.paste(S.to_image(a), (x, yy))
        img.paste(S.to_image(b), (x, yy + a.shape[0] + 2))
        xx = x + a.shape[1] + pad
        img.paste(S.to_image(a, zoom), (xx, yy))
        img.paste(S.to_image(b, zoom), (xx + a.shape[1] * zoom + pad, yy))
    img.save(out)
    print(out, img.size)


if __name__ == "__main__":
    main()

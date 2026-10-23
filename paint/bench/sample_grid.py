#!/usr/bin/env python3
"""Draw what the Mac would draw: sample complete drawings from scratch with the bit-exact
integer reference (ref.finish_it with an empty prompt) and render them as a PNG grid.

Usage: sample_grid.py <model.dmdl> <out.png> [--cats cat,bicycle,...] [--n 4] [--temp 1]
"""
import argparse
import os
import sys

from PIL import Image, ImageDraw

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import model_file  # noqa: E402
import ref  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("model")
ap.add_argument("out")
ap.add_argument("--cats", default="cat,bicycle,house,tree,fish,car,face,flower,airplane,The Eiffel Tower")
ap.add_argument("--n", type=int, default=4)
ap.add_argument("--temp", type=int, default=1, help="0 tidy 0.3, 1 normal 0.5, 2 wild 0.8")
a = ap.parse_args()
m = model_file.read(a.model)
cats = [c.strip() for c in a.cats.split(",")]
cell, pad = 128, 4
img = Image.new("1", (cell * a.n, (cell + 12) * len(cats)), 1)
d = ImageDraw.Draw(img)
for ci, name in enumerate(cats):
    if name not in m["names"]:
        print("no category", name)
        continue
    cat = m["names"].index(name)
    for j in range(a.n):
        gen = ref.finish_it(m, [], cat, 1000 + 17 * j + 1, a.temp)
        x = y = 0
        pts, pen = [(0, 0)], 0
        segs = []
        for dx, dy, p in gen:
            if pen == 0:
                segs.append((x, y, x + dx, y + dy))
            x, y, pen = x + dx, y + dy, p
            pts.append((x, y))
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        span = max(max(xs) - min(xs), max(ys) - min(ys), 1)
        s = (cell - 2 * pad) / span
        ox, oy = j * cell + pad - min(xs) * s, ci * (cell + 12) + 12 + pad - min(ys) * s
        for x0, y0, x1, y1 in segs:
            d.line((ox + x0 * s, oy + y0 * s, ox + x1 * s, oy + y1 * s), fill=0)
    d.text((2, ci * (cell + 12)), f"{name} ({len(gen)} moves)", fill=0)
img.save(a.out)
print("wrote", a.out)

#!/usr/bin/env python3
"""Contact sheet of ground-level views around the island centre (for tuning the look).
    look.py [--seed 1] [--lines 1] [--out build/m0/look.png] [--x 32 --z 32]"""
import argparse, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "engine"))
sys.path.insert(0, HERE)
from PIL import Image, ImageDraw
import render as R, world as W, views as V

ap = argparse.ArgumentParser()
ap.add_argument("--seed", type=int, default=1)
ap.add_argument("--lines", type=int, default=1)
ap.add_argument("--x", type=int, default=32)
ap.add_argument("--z", type=int, default=32)
ap.add_argument("--dmax", type=int, default=16)
ap.add_argument("--h", type=int, default=288)
ap.add_argument("--out", default=os.path.join(HERE, "..", "build", "m0", "look.png"))
a = ap.parse_args()
b, hm = W.generate(a.seed)
at = R.make_seg_lookup(W.all_segments(b))
cfg = R.Cfg(lines=a.lines, dmax=a.dmax, h=a.h)
x, z = a.x, a.z
# step off trees
def ok(x, z):
    return all(W.column_segments(b, x + dx, z + dz)[0][2] == W.TURF for dx in (-1, 0, 1) for dz in (-1, 0, 1))
cands = sorted(((xx - x) ** 2 + (zz - z) ** 2, xx, zz) for xx in range(4, 60) for zz in range(4, 60))
x, z = next((xx, zz) for _, xx, zz in cands if ok(xx, zz))
g = V.ground(b, x, z)
shots = []
for i, (yaw, pitch) in enumerate([(0, -10), (256, -10), (512, -10), (768, -10), (128, -90), (640, 60)]):
    cam = R.Cam((x << 8) + 128, (g << 8) + 410, (z << 8) + 128, yaw, pitch)
    shots.append((f"yaw {yaw} pitch {pitch}", V.to_img(R.render(at, cam, cfg))))
sheet = Image.new("L", (2 * (cfg.W + 8), 3 * (cfg.H + 16)), 255)
d = ImageDraw.Draw(sheet)
for i, (n, im) in enumerate(shots):
    x0, y0 = (i % 2) * (cfg.W + 8), (i // 2) * (cfg.H + 16)
    sheet.paste(im, (x0, y0 + 14)); d.text((x0 + 2, y0 + 1), n, fill=0)
sheet.save(a.out)
print("camera", x, z, "ground", g, "->", a.out)

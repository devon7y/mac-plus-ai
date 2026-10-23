#!/usr/bin/env python3
"""Render the test scene and island views (1-bit + false colour) into one sheet: compare.py OUT.png [lines]"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "engine")); sys.path.insert(0, HERE)
from PIL import Image
import world as W, render as R, views as V, falsecolor as FC, testscene as TS
out = sys.argv[1]; lines = int(sys.argv[2]) if len(sys.argv) > 2 else 1
cfg = R.Cfg(lines=lines)
shots = []
tb = TS.build(); tat = R.make_seg_lookup(W.all_segments(tb))
for cam in [R.Cam((30 << 8) + 128, (11 << 8) + 410, (30 << 8) + 128, 20, -10),
            R.Cam((32 << 8) + 128, (11 << 8) + 410, (38 << 8) + 128, 80, -30)]:
    shots.append((tat, cam))
b, hm = W.generate(4); at = R.make_seg_lookup(W.all_segments(b))
for (x, z, yaw, pitch) in [(24, 40, 900, -10), (40, 20, 300, -20)]:
    g = V.ground(b, x, z)
    shots.append((at, R.Cam((x << 8) + 128, (g << 8) + 410, (z << 8) + 128, yaw, pitch)))
sh = Image.new("RGB", (392 * 2, 296 * len(shots)), (255, 255, 255))
for i, (a, cam) in enumerate(shots):
    ids, keys = R.render_cols(a, cam, cfg)
    sh.paste(V.to_img(R.to_bitmap(ids, keys, cfg)).convert("RGB"), (0, i * 296))
    sh.paste(Image.fromarray(FC.falsecolor(ids, cfg)), (392, i * 296))
sh.save(out)

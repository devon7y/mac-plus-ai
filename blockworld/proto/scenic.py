#!/usr/bin/env python3
"""Pick scenic ground-level viewpoints by scoring random views, render them: scenic.py [--seed 8] [--n 6]"""
import argparse, math, os, random, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "engine")); sys.path.insert(0, HERE)
from PIL import Image, ImageDraw
import world as W, render as R, views as V, falsecolor as FC, patterns as PT

ap = argparse.ArgumentParser()
ap.add_argument("--seed", type=int, default=8)
ap.add_argument("--n", type=int, default=6)
ap.add_argument("--tries", type=int, default=300)
ap.add_argument("--out", default=os.path.join(HERE, "..", "build", "m0", "scenic"))
ap.add_argument("--nc", type=int, default=96)
ap.add_argument("--colw", type=int, default=4)
ap.add_argument("--h", type=int, default=288)
ap.add_argument("--dmax", type=int, default=16)
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
b, hm = W.generate(a.seed)
at = R.make_seg_lookup(W.all_segments(b))
cfg = R.Cfg(nc=a.nc, colw=a.colw, h=a.h, dmax=a.dmax)
rng = random.Random(a.seed)
cands = []
for _ in range(a.tries):
    x, z = rng.randrange(6, W.WX - 6), rng.randrange(6, W.WX - 6)
    if W.column_segments(b, x, z)[0][2] not in (W.TURF, W.SAND):
        continue
    yaw = rng.randrange(1024)
    pitch = rng.choice((-30, -15, 0, 10))
    cam = R.Cam((x << 8) + 128, (V.ground(b, x, z) << 8) + 415, (z << 8) + 128, yaw, pitch)
    ids, keys = R.render_cols(at, cam, cfg)
    cnt = {}
    for col in ids:
        for i in col[::4]:
            t = i >> 5 if i >= 32 else (0 if i < 4 or i > 11 else W.TURF)
            cnt[t] = cnt.get(t, 0) + 1
    n = sum(cnt.values())
    fr = {t: c / n for t, c in cnt.items()}
    if not (0.2 < fr.get(0, 0) < 0.5) or fr.get(W.LEAVES, 0) > 0.25:
        continue
    ent = -sum(p * math.log(p) for p in fr.values() if p > 0)
    score = ent + 2 * min(fr.get(W.LEAVES, 0), 0.1) + 2 * min(fr.get(W.WATER, 0), 0.1)
    cands.append((score, x, z, yaw, pitch, cam, (ids, keys)))
cands.sort(key=lambda c: -c[0])
picked = []
for c in cands:
    if all(abs(c[1] - p[1]) + abs(c[2] - p[2]) > 8 for p in picked):
        picked.append(c)
    if len(picked) == a.n:
        break
sheet = Image.new("RGB", (2 * (cfg.W + 8), len(picked) * (cfg.H + 16)), (255, 255, 255))
d = ImageDraw.Draw(sheet)
for i, (score, x, z, yaw, pitch, cam, (ids, keys)) in enumerate(picked):
    im = V.to_img(R.to_bitmap(ids, keys, cfg))
    im.save(os.path.join(a.out, f"view{i}.png"))
    y0 = i * (cfg.H + 16)
    sheet.paste(im.convert("RGB"), (0, y0 + 14))
    sheet.paste(Image.fromarray(FC.falsecolor(ids, cfg)), (cfg.W + 8, y0 + 14))
    d.text((2, y0 + 1), f"seed {a.seed}  x {x} z {z} yaw {yaw} pitch {pitch}", fill=(0, 0, 0))
    print(f"view{i}: x {x} z {z} yaw {yaw} pitch {pitch} score {score:.2f}")
sheet.save(os.path.join(a.out, "sheet.png"))

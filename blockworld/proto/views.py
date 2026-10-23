#!/usr/bin/env python3
"""Render views of a generated world with the reference renderer.

    views.py [--seed 1] [--out DIR] [--lines 1] [--nc 96 --colw 4 --h 288] [--dmax 16]

Writes view_*.png (1:1 Mac pixels) and sheet.png (all views, labelled) to OUT.
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "engine"))
from PIL import Image, ImageDraw  # noqa: E402

import render as R  # noqa: E402
import world as W  # noqa: E402


def ground(b, x, z):
    """y of the surface (top of the highest solid or water block) at block (x, z)."""
    for y in range(W.WY - 1, -1, -1):
        if b[W.idx(x, z, y)] != W.AIR:
            return y + 1
    return 0


def to_img(bits):
    return Image.fromarray((1 - bits) * 255).convert("L")


def find_spots(b, hm):
    """A few interesting camera positions, found from the world."""
    spots = {}
    def open_turf(i):       # turf on top, nothing above it, no leaves around
        x, z = i // W.WX, i % W.WX
        if not (3 <= x < W.WX - 3 and 3 <= z < W.WX - 3):
            return False
        for dx in (-2, 0, 2):
            for dz in (-2, 0, 2):
                s = W.column_segments(b, x + dx, z + dz)
                if s[0][2] in (W.LEAVES, W.LOG):
                    return False
        return W.column_segments(b, x, z)[0][2] == W.TURF
    # highest open point
    best = max((i for i in range(W.WX * W.WX) if open_turf(i)), key=lambda i: hm[i])
    spots["hilltop"] = (best // W.WX, best % W.WX)
    # a tree: first log found scanning from the centre outwards
    for r in range(40):
        for x in range(32 - r, 33 + r):
            for z in (32 - r, 32 + r):
                if 0 <= x < 64 and 0 <= z < 64 and any(b[W.idx(x, z, y)] == W.LOG for y in range(W.WY)):
                    spots.setdefault("tree", (x, z))
        if "tree" in spots:
            break
    # a beach: sand column next to water near the middle of an edge
    for x in range(10, 54):
        z = 32
        if b[W.idx(x, z, ground(b, x, z) - 1)] == W.SAND:
            spots["beach"] = (x, z)
            break
    # a cave: air under the surface, open to the sky somewhere
    for x in range(2, 62):
        for z in range(2, 62):
            g = hm[x * W.WX + z]
            if g > W.SEA + 2 and b[W.idx(x, z, g)] == W.AIR and b[W.idx(x, z, g - 1)] == W.AIR:
                spots.setdefault("cave", (x, z))
    return spots


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", default=os.path.join(HERE, "..", "build", "m0"))
    ap.add_argument("--lines", type=int, default=1)
    ap.add_argument("--nc", type=int, default=96)
    ap.add_argument("--colw", type=int, default=4)
    ap.add_argument("--h", type=int, default=288)
    ap.add_argument("--dmax", type=int, default=16)
    ap.add_argument("--tag", default="")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    b, hm = W.generate(a.seed)
    segs = W.all_segments(b)
    at = R.make_seg_lookup(segs)
    cfg = R.Cfg(nc=a.nc, colw=a.colw, h=a.h, dmax=a.dmax, lines=a.lines)
    sp = find_spots(b, hm)
    print("spots", sp)
    views = []
    eye = 410            # 1.6 blocks above the feet
    hx, hz = sp["hilltop"]
    views.append(("hilltop, looking out", R.Cam((hx << 8) + 128, (ground(b, hx, hz) << 8) + eye, (hz << 8) + 128, 640, -20)))
    views.append(("hilltop, other way", R.Cam((hx << 8) + 128, (ground(b, hx, hz) << 8) + eye, (hz << 8) + 128, 128, -20)))
    tx, tz = sp["tree"]
    cx, cz = tx - 5, tz - 2
    for cx, cz in ((tx - 5, tz - 2), (tx + 5, tz + 2), (tx - 2, tz - 5), (tx + 2, tz + 5)):
        if W.column_segments(b, cx, cz)[0][2] in (W.TURF, W.SAND, W.SOIL):
            break
    views.append(("near a tree", R.Cam((cx << 8) + 128, (ground(b, cx, cz) << 8) + eye, (cz << 8) + 128, 60, 10)))
    if "beach" in sp:
        bx, bz = sp["beach"]
        views.append(("beach, looking inland", R.Cam((bx << 8) + 128, (ground(b, bx, bz) << 8) + eye, (bz << 8) + 128, 0, 0)))
    views.append(("looking down", R.Cam((hx << 8) + 128, (ground(b, hx, hz) << 8) + eye, (hz << 8) + 128, 900, -200)))
    views.append(("looking up at a tree", R.Cam((tx - 2 << 8) + 128, (ground(b, tx - 2, tz) << 8) + eye, (tz << 8) + 128, 0, 160)))
    if "cave" in sp:
        vx, vz = sp["cave"]
        g = hm[vx * W.WX + vz]
        y = g
        while y > 1 and b[W.idx(vx, vz, y - 1)] == W.AIR:
            y -= 1
        views.append(("in a cave", R.Cam((vx << 8) + 128, (y << 8) + eye, (vz << 8) + 128, 300, 0)))
    imgs = []
    for i, (name, cam) in enumerate(views):
        st = {}
        t = time.time()
        bits = R.render(at, cam, cfg, st)
        dt = time.time() - t
        im = to_img(bits)
        fn = os.path.join(a.out, f"view{a.tag}_{i}.png")
        im.save(fn)
        imgs.append((name, im))
        print(f"{name:24s} cells {st['cells']:5d} ({st['cells'] / cfg.NC:.1f}/col)  {dt * 1000:.0f} ms  -> {fn}")
    cols = 2
    rows = (len(imgs) + cols - 1) // cols
    W_, H_ = cfg.W, cfg.H
    sheet = Image.new("L", (cols * (W_ + 8), rows * (H_ + 20)), 255)
    d = ImageDraw.Draw(sheet)
    for i, (name, im) in enumerate(imgs):
        x0, y0 = (i % cols) * (W_ + 8), (i // cols) * (H_ + 20)
        sheet.paste(im, (x0, y0 + 16))
        d.text((x0 + 2, y0 + 2), name, fill=0)
    sheet.save(os.path.join(a.out, f"sheet{a.tag}.png"))


if __name__ == "__main__":
    main()

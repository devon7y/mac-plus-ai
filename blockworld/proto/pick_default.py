#!/usr/bin/env python3
"""Pick the shipped starting world: scan seeds and standing spots for a view with trees, water, a
hill and a cave mouth or ravine in sight, then render the best ones as the game shows them at
first (Full 448 x 284, view distance 10, day light) with the reference renderer.

    pick_default.py [--seeds 1-80] [--n 12] [--out build/m7/pick]

Writes OUT/sheet.png (one labelled render per candidate, best first) and OUT/cands.txt
(seed x z yaw aim score features). The spot is a turf block; the camera is the player's eye.
"""
import argparse
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "engine"))
sys.path.insert(0, HERE)
from PIL import Image, ImageDraw  # noqa: E402

import render as R  # noqa: E402
import world as W  # noqa: E402
import views as V  # noqa: E402

DAY_SHADE = (0, 2, 3, 0)          # game.c SHADE[0]
NC, H, DMAX = 112, 284, 10        # the default view on a Plus (Full, Normal distance)
HALF = math.atan(224 / 256)       # half the horizontal field of view
PITCH = -24                       # the view a little down (rows of y-shear)


def column_info(b, hm):
    """per column: its top, a log in it, water on top, carved (its surface cut away by a cave or
    ravine: lower than the height map by 3 or more, not under water)"""
    top, log, water, carved = [0] * (W.WX * W.WX), [0] * (W.WX * W.WX), [0] * (W.WX * W.WX), [0] * (W.WX * W.WX)
    for c in range(W.WX * W.WX):
        col = b[c * W.WY:(c + 1) * W.WY]
        y = W.WY
        while y > 0 and col[y - 1] == W.AIR:
            y -= 1
        top[c] = y
        log[c] = W.LOG in col
        water[c] = y > 0 and col[y - 1] == W.WATER
        carved[c] = hm[c] > W.SEA + 1 and y < hm[c] - 2 and not water[c]
    return top, log, water, carved


RAYS = [(d, k) for d in range(2, 13) for k in range(-4, 5)]


def features(info, x, z, yaw):
    """what a player at (x, z) facing yaw sees, roughly (no occlusion): counts per feature"""
    top, log, water, carved = info
    feet = top[x * W.WX + z]
    a0 = yaw * 2 * math.pi / 1024
    trees, wat, cave, hill, rough, n_fg = set(), set(), set(), -99, 0, 0
    for d, k in RAYS:
        a = a0 + HALF * 0.9 * k / 4
        cx, cz = int(x + 0.5 + d * math.cos(a)), int(z + 0.5 + d * math.sin(a))
        if not (0 <= cx < W.WX and 0 <= cz < W.WX):
            continue
        c = cx * W.WX + cz
        t = top[c]
        if d <= 4:                                     # the foreground: open, near the feet
            n_fg += 1
            if (abs(t - feet) > 1 and not water[c]) or t > feet + 2:
                rough += 1
            if log[c] or t > feet + 2:
                rough += 1                             # (a tree or a wall in the face)
        if log[c] and 6 <= d <= 11:
            trees.add(c)
        if water[c] and d > 3:
            wat.add(c)
        if 6 <= d <= 12:
            hill = max(hill, t - feet)
        if 4 <= d <= 9 and carved[c]:
            cave.add(c)
    return dict(trees=len(trees), water=len(wat), hill=hill, cave=len(cave), rough=rough / max(n_fg, 1))


def score(f):
    if f["rough"] > 0.25 or not (f["trees"] and f["water"] >= 8 and f["cave"] >= 2 and 3 <= f["hill"] <= 9):
        return -1
    return min(f["trees"], 3) + min(f["water"], 30) / 10 + min(f["cave"], 10) / 4 + (2 if 4 <= f["hill"] <= 7 else 0)


def composition(ids):
    """fractions of the view: sky, water, trees (leaves, logs), dark (shaded: inside caves), far hills"""
    n = sky = water = tree = dark = far = 0
    for col in ids:
        for i in col[::2]:
            n += 1
            if i < 32:
                if i <= 2:
                    sky += 1
                elif 12 <= i <= 14:
                    far += 1
                continue
            t = i >> 5
            if t == W.WATER:
                water += 1
            elif t in (W.LEAVES, W.LOG):
                tree += 1
            if (i & 3) == 3:
                dark += 1
    return dict(sky=sky / n, water=water / n, tree=tree / n, dark=dark / n, far=far / n)


def view_score(c):
    sc = 0.0
    sc += 3 - abs(c["sky"] - 0.32) * 10                 # about a third sky
    sc += min(c["water"], 0.15) * 20
    sc += min(c["tree"], 0.12) * 25
    sc += min(c["dark"], 0.04) * 75                     # a cave mouth's dark inside
    sc += min(c["far"], 0.05) * 20
    return sc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", default="1-80")
    ap.add_argument("--n", type=int, default=12)
    ap.add_argument("--out", default=os.path.join(HERE, "..", "build", "m7", "pick"))
    a = ap.parse_args()
    lo, hi = (int(v) for v in a.seeds.split("-"))
    os.makedirs(a.out, exist_ok=True)
    cands = []
    qcfg = R.Cfg(nc=NC // 2, colw=8, h=H, dmax=DMAX)    # (a quicker render to score the view)
    for seed in range(lo, hi + 1):
        b, hm = W.generate(seed)
        b = bytes(b)
        info = column_info(b, hm)
        top = info[0]
        spots = []
        for x in range(16, W.WX - 16, 3):
            for z in range(16, W.WX - 16, 3):
                c = x * W.WX + z
                t = top[c]
                if t == 0 or b[c * W.WY + t - 1] != W.TURF or t > W.WY - 4:
                    continue
                for yaw in range(0, 1024, 64):
                    f = features(info, x, z, yaw)
                    sc = score(f)
                    if sc > 0:
                        spots.append((sc, x, z, yaw, f))
        spots.sort(key=lambda v: -v[0])
        if not spots:
            continue
        at = R.make_seg_lookup(W.all_segments(b))
        farh = W.make_farh(W.coarse_heights(b))
        best = None
        for sc, x, z, yaw, f in spots[:5]:
            cam = R.Cam((x << 8) + 128, (top[x * W.WX + z] << 8) + 415, (z << 8) + 128, yaw, PITCH)
            ids, _ = R.render_cols(at, cam, qcfg, farh=farh, shade=DAY_SHADE)
            comp = composition(ids)
            v = view_score(comp) + sc / 4
            if best is None or v > best[0]:
                best = (v, seed, x, z, yaw, dict(f, **{k: round(val, 3) for k, val in comp.items()}))
        cands.append(best)
        print("seed %3d: %.2f at (%d, %d) yaw %d %s" % (best[1], best[0], best[2], best[3], best[4], best[5]), flush=True)
    cands.sort(key=lambda c: -c[0])
    cands = cands[:a.n]
    cfg = R.Cfg(nc=NC, h=H, dmax=DMAX)
    cols = 3
    sheet = Image.new("L", (cols * (cfg.W + 8), ((len(cands) + cols - 1) // cols) * (cfg.H + 16)), 255)
    d = ImageDraw.Draw(sheet)
    lines = []
    for i, (s, seed, x, z, yaw, f) in enumerate(cands):
        b, hm = W.generate(seed)
        segs = W.all_segments(b)
        at = R.make_seg_lookup(segs)
        farh = W.make_farh(W.coarse_heights(b))
        feet = V.ground(b, x, z)
        cam = R.Cam((x << 8) + 128, (feet << 8) + 415, (z << 8) + 128, yaw, PITCH)
        im = V.to_img(R.render(at, cam, cfg, farh=farh, shade=DAY_SHADE))
        im.save(os.path.join(a.out, f"seed{seed}.png"))
        px, py = (i % cols) * (cfg.W + 8), (i // cols) * (cfg.H + 16)
        sheet.paste(im, (px, py + 14))
        d.text((px + 2, py + 1), f"seed {seed} ({x},{z}) yaw {yaw} score {s:.1f} sky {f['sky']:.2f} water {f['water']:.2f} tree {f['tree']:.2f} dark {f['dark']:.3f}", fill=0)
        lines.append(f"{seed} {x} {z} {yaw} {PITCH} {s:.2f} {f}")
    sheet.save(os.path.join(a.out, "sheet.png"))
    open(os.path.join(a.out, "cands.txt"), "w").write("\n".join(lines) + "\n")
    print("wrote", os.path.join(a.out, "sheet.png"))


if __name__ == "__main__":
    main()

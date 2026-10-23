#!/usr/bin/env python3
"""Pick the starting world by its cave mouths: a tunnel cell (air under solid ground) whose
neighbour on one side is open to the sky at that height. The camera stands on turf 5 to 8
blocks out from the opening, facing it, with the mouth in plain sight (no column between rises
above the line of sight); the views are scored by what the reference renderer shows (sky, water,
trees, the dark inside) and the best are rendered as the game's first frame (Full 448 x 284,
view 10, day).

    pick_mouth.py [--seeds 1-240] [--n 12] [--out build/m7/mouth]

Writes OUT/sheet.png and OUT/cands.txt (seed x z yaw aim score ...).
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
import patterns as PT  # noqa: E402

# the game's day look (game.c set_sky(0)): plain white sky, its haze and fog, its distant hills
PT.PAT[PT.SKY] = [0] * 8
PT.PAT[PT.HAZE] = [0, 0, 0, 0, 0x40, 0, 0, 0]
PT.PAT[PT.FOGBG] = [0, 0, 0, 0, 0x40, 0, 0x04, 0]
PT.PAT[PT.FARHILL] = [0x88, 0, 0x20, 0, 0x88, 0, 0x02, 0]
PT.PAT[PT.FARHILL + 1] = [0x88, 0, 0, 0, 0x88, 0, 0, 0]
PT.PAT[PT.FARLINE] = [0x22, 0x88, 0x22, 0x88, 0x22, 0x88, 0x22, 0x88]
from pick_default import column_info, composition, features, DAY_SHADE, NC, H, DMAX  # noqa: E402

DIRS = ((1, 0), (-1, 0), (0, 1), (0, -1))


SOLID_T = {W.ROCK, W.SOIL, W.TURF, W.SAND, W.GRAVEL, W.CLAY, W.COAL, W.IRON, W.GOLD, W.GEM}


def mouths(b, top):
    """(x, y, z, dx, dz): air at (x, y, z) and y + 1 with terrain above (a tunnel, not a tree's
    shade), on a floor, and the column (x + dx, z + dz) open to the sky from y + 1 up (the outside)"""
    out = []
    for x in range(10, W.WX - 10):
        for z in range(10, W.WX - 10):
            c = x * W.WX + z
            t = top[c]
            for y in range(W.SEA - 2, t - 2):
                if b[c * W.WY + y] != W.AIR or b[c * W.WY + y + 1] != W.AIR or b[c * W.WY + y - 1] in (W.AIR, W.WATER, W.LAVA):
                    continue
                yy = y + 2
                while yy < W.WY and b[c * W.WY + yy] == W.AIR:
                    yy += 1
                if yy >= W.WY or b[c * W.WY + yy] not in SOLID_T:
                    continue
                for dx, dz in DIRS:
                    n = (x + dx) * W.WX + (z + dz)
                    if top[n] <= y + 1:
                        out.append((x, y, z, dx, dz))
    return out


def clear_line(top, x0, z0, e0, x1, z1, e1):
    """the line of sight from eye height e0 at (x0, z0) to height e1 at (x1, z1) clears every column"""
    n = int(max(abs(x1 - x0), abs(z1 - z0)) * 3) + 1
    for i in range(1, n):
        f = i / n
        x, z, e = x0 + (x1 - x0) * f, z0 + (z1 - z0) * f, e0 + (e1 - e0) * f
        if int(x) == int(x1) and int(z) == int(z1):
            break                                      # (at the opening: the tunnel's own column)
        if top[int(x) * W.WX + int(z)] > e:
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", default="1-240")
    ap.add_argument("--n", type=int, default=12)
    ap.add_argument("--out", default=os.path.join(HERE, "..", "build", "m7", "mouth"))
    a = ap.parse_args()
    lo, hi = (int(v) for v in a.seeds.split("-"))
    os.makedirs(a.out, exist_ok=True)
    qcfg = R.Cfg(nc=NC // 2, colw=8, h=H, dmax=DMAX)
    cands = []
    for seed in range(lo, hi + 1):
        b, hm = W.generate(seed)
        b = bytes(b)
        info = column_info(b, hm)
        top = info[0]
        spots = []
        for mx, my, mz, dx, dz in mouths(b, top):
            for dist in (5, 6, 7, 8):
                for side in (-2, -1, 0, 1, 2):
                    cx, cz = mx + dx * dist + dz * side, mz + dz * dist + dx * side
                    if not (3 <= cx < W.WX - 3 and 3 <= cz < W.WX - 3):
                        continue
                    c = cx * W.WX + cz
                    t = top[c]
                    if t == 0 or b[c * W.WY + t - 1] != W.TURF or not (t - 2 <= my <= t + 2):
                        continue
                    eye = t + 1.62
                    if not clear_line(top, cx + 0.5, cz + 0.5, eye, mx + 0.5, mz + 0.5, my + 1):
                        continue
                    yaw = int(round(math.atan2(mz - cz, mx - cx) * 1024 / (2 * math.pi))) & 1023
                    spots.append((seed, cx, cz, yaw, mx, my, mz))
        if not spots:
            continue
        at = R.make_seg_lookup(W.all_segments(b))
        farh = W.make_farh(W.coarse_heights(b))
        best = None
        ranked, seen = [], set()
        for sp in spots:                       # (rank by the cheap look first: trees, water, a hill)
            seed_, cx, cz, yaw, mx, my, mz = sp
            if (cx, cz, yaw) in seen:
                continue
            seen.add((cx, cz, yaw))
            f = features(info, cx, cz, yaw)
            ranked.append((min(f["trees"], 2) * 2 + min(f["water"], 20) / 5 + (1 if 3 <= f["hill"] <= 8 else 0)
                           - f["rough"] * 4, sp))
        ranked.sort(key=lambda r: -r[0])
        for _, sp in ranked[:6]:
            seed_, cx, cz, yaw, mx, my, mz = sp
            pitch = -12
            cam = R.Cam((cx << 8) + 128, (top[cx * W.WX + cz] << 8) + 415, (cz << 8) + 128, yaw, pitch)
            ids, _ = R.render_cols(at, cam, qcfg, farh=farh, shade=DAY_SHADE)
            comp = composition(ids)
            sc = (2.5 - abs(comp["sky"] - 0.32) * 8 + min(comp["water"], 0.12) * 25 + min(comp["tree"], 0.12) * 25
                  + min(comp["dark"], 0.05) * 60 - max(0, comp["tree"] - 0.2) * 30
                  - (8 if comp["sky"] < 0.18 or comp["dark"] < 0.02 else 0))
            if best is None or sc > best[0]:
                best = (sc, seed, cx, cz, yaw, pitch, (mx, my, mz), comp)
        cands.append(best)
        print("seed %3d: %.2f at (%d, %d) yaw %d pitch %d mouth %s %s" % (best[1], best[0], best[2], best[3], best[4], best[5], best[6],
              {k: round(v, 3) for k, v in best[7].items()}), flush=True)
    cands.sort(key=lambda c: -c[0])
    cands = cands[:a.n]
    cfg = R.Cfg(nc=NC, h=H, dmax=DMAX)
    cols = 3
    sheet = Image.new("L", (cols * (cfg.W + 8), ((len(cands) + cols - 1) // cols) * (cfg.H + 16)), 255)
    d = ImageDraw.Draw(sheet)
    lines = []
    for i, (sc, seed, x, z, yaw, pitch, m, comp) in enumerate(cands):
        b, hm = W.generate(seed)
        at = R.make_seg_lookup(W.all_segments(b))
        farh = W.make_farh(W.coarse_heights(b))
        cam = R.Cam((x << 8) + 128, (V.ground(b, x, z) << 8) + 415, (z << 8) + 128, yaw, pitch)
        im = V.to_img(R.render(at, cam, cfg, farh=farh, shade=DAY_SHADE))
        px, py = (i % cols) * (cfg.W + 8), (i // cols) * (cfg.H + 16)
        sheet.paste(im, (px, py + 14))
        d.text((px + 2, py + 1), f"seed {seed} ({x},{z}) yaw {yaw} pitch {pitch} mouth {m} {sc:.1f}", fill=0)
        lines.append(f"{seed} {x} {z} {yaw} {pitch} {sc:.2f} {m} {comp}")
    sheet.save(os.path.join(a.out, "sheet.png"))
    open(os.path.join(a.out, "cands.txt"), "w").write("\n".join(lines) + "\n")
    print("wrote", os.path.join(a.out, "sheet.png"))


if __name__ == "__main__":
    main()

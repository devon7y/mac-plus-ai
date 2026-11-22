#!/usr/bin/env python3
"""Watch policies play: a game in the numpy twin with integer policies (or bots), drawn with the
game's own sprites into an animated GIF (one frame every 2 ticks).

    watch.py OUT.gif --a rusher=path.npz --b player=hunter [--arena 3] [--seconds 20] [--seed 1] [--ai]

--a / --b: <tank type>=<policy .npz | .pt | hunter | sitter | wander>; tank a is team 0 at the
player's spawn, b is team 1 at spawn 'a'. Add more enemies with --c. --ai draws the AI view dots.
"""
import argparse
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
TANKS = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(TANKS, "sim"))
sys.path.insert(0, os.path.join(TANKS, "train"))
sys.path.insert(0, os.path.join(TANKS, "app"))
import defs as D  # noqa: E402
from twin import Twin, ARENA_SPAWNS, COS_A, SIN_A  # noqa: E402
from model import int_forward  # noqa: E402
from evaluate import load_policy, TYPES, BOTS, act_of  # noqa: E402
import gen_art as G  # noqa: E402


def sprites():
    sh = G.Sheet()
    G.make_sprites(sh)
    out = []
    for (name, w, h, ox, oy, ns, off), (mask, img) in zip(sh.sprites, sh.preview):
        out.append((ox, oy, mask, img))
    return out


def background(grid):
    walls = [G.wall_tile(bool(m & 1), bool(m & 2), bool(m & 4), bool(m & 8)) for m in range(16)]
    floors = [G.floor_tile(bool(m & 1), bool(m & 2), bool(m & 4)) for m in range(8)]
    bg = np.zeros((288, 384), np.uint8)

    def W(x, y):
        return x < 0 or y < 0 or x >= 24 or y >= 18 or grid[y][x]
    for ty in range(18):
        for tx in range(24):
            if grid[ty][tx]:
                m = (0 if W(tx, ty - 1) else 1) | (0 if W(tx + 1, ty) else 2) | (0 if W(tx, ty + 1) else 4) | (0 if W(tx - 1, ty) else 8)
                t = walls[m]
            else:
                m = (1 if W(tx - 1, ty) else 0) | (2 if W(tx, ty - 1) else 0) | (4 if W(tx - 1, ty - 1) else 0)
                t = floors[m]
            bg[ty * 16:ty * 16 + 16, tx * 16:tx * 16 + 16] = t
    return bg


def blit(fr, spr, cx, cy):
    ox, oy, mask, img = spr
    h, w = mask.shape
    y0, x0 = cy - oy, cx - ox
    sub = fr[y0:y0 + h, x0:x0 + w]
    m = mask[:sub.shape[0], :sub.shape[1]]
    sub[m] = img[:sub.shape[0], :sub.shape[1]][m]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--a", required=True)
    ap.add_argument("--b", required=True)
    ap.add_argument("--c", action="append", default=[])
    ap.add_argument("--arena", type=int, default=0)
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--ai", action="store_true")
    a = ap.parse_args()
    specs = [a.a, a.b] + a.c
    ctl, types = [], []
    for s in specs:
        t, p = s.split("=", 1)
        types.append(TYPES[t])
        ctl.append(BOTS[p] if p in BOTS else load_policy(p))
    nt = len(specs)
    sp = ARENA_SPAWNS[a.arena]
    keys = ["P", "a", "b", "c"]
    tw = Twin(1, nt)
    tw.init(np.array([a.arena]), np.array([a.seed]))
    for i in range(nt):
        tx, ty = sp[keys[i]]
        ang = 0 if i == 0 else 128
        tw.set_tank(i, types[i], 0 if i == 0 else 1, tx * 256 + 128, ty * 256 + 128, ang)
    S = sprites()
    bg = background(D.arena(a.arena)[1])
    frames = []
    from PIL import Image
    for k in range(int(a.seconds * 30)):
        for i in range(nt):
            if tw.t["alive"][0, i] and k % D.DECIDE == i % D.DECIDE:
                if isinstance(ctl[i], int):
                    tw.bot_act(i, ctl[i])
                else:
                    tw.apply_action(i, act_of(tw, ctl[i], tw.obs(i)))
        tw.step()
        if k % 2:
            continue
        fr = bg.copy()
        t = tw.t
        if a.ai:
            for i in range(nt):
                if not t["alive"][0, i] or isinstance(ctl[i], int):
                    continue
                x, y, h = int(t["x"][0, i]), int(t["y"][0, i]), int(t["hull"][0, i])
                for r in range(8):
                    ang = (h + 32 * r) & 255
                    dx = (128 * int(COS_A[ang]) + 8192) >> 14
                    dy = (128 * int(SIN_A[ang]) + 8192) >> 14
                    px, py = x, y
                    for n in range(1, 21):
                        px += dx
                        py += dy
                        if tw.map[0, py >> 8, px >> 8]:
                            break
                        fr[py >> 4, px >> 4] ^= 1
        for i in range(nt):
            if not t["alive"][0, i]:
                continue
            x, y = int(t["x"][0, i]) >> 4, int(t["y"][0, i]) >> 4
            d = int(t["type"][0, i])
            ph = (((int(t["x"][0, i]) + int(t["y"][0, i])) >> 6) ^ (int(t["hull"][0, i]) >> 3)) & 1
            blit(fr, S[(d * 2 + ph) * 32 + ((int(t["hull"][0, i]) + 4) >> 3 & 31)], x, y)
            blit(fr, S[256 + d * 32 + ((int(t["tur"][0, i]) + 4) >> 3 & 31)], x, y)
        s = tw.s
        for j in range(tw.ns):
            if s["alive"][0, j]:
                blit(fr, S[384], int(s["x"][0, j]) >> 4, int(s["y"][0, j]) >> 4)
        frames.append(Image.fromarray(((1 - fr) * 255).astype(np.uint8)).resize((768, 576), Image.NEAREST))
        al = t["alive"][0]
        tm = t["team"][0]
        if not (al[tm == 0].any() and al[tm == 1].any()):
            for _ in range(10):
                frames.append(frames[-1])
            break
    frames[0].save(a.out, save_all=True, append_images=frames[1:], duration=66, loop=0)
    al = tw.t["alive"][0]
    print(f"{len(frames)} frames, tick {int(tw.tick[0])}, alive {al.tolist()} -> {a.out}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""The C player (app/player.c) against engine/player.py on random input scripts (seed-2 world).
    check_player.py [--n 40] [--steps 400]"""
import argparse, os, random, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "engine")); sys.path.insert(0, os.path.join(ROOT, "proto"))
import world as W, render as R, player as PL, views as V
PL.SIN, PL.COS = R.SIN, R.COS
ap = argparse.ArgumentParser()
ap.add_argument("--n", type=int, default=40)
ap.add_argument("--steps", type=int, default=400)
ap.add_argument("--props", action="store_true", help="fences, ladders and lava strewn on the ground")
a = ap.parse_args()
out = os.path.join(ROOT, "build", "host")
os.makedirs(out, exist_ok=True)
gen = os.path.join(ROOT, "build", "gen")
exe = os.path.join(out, "player_test")
subprocess.run(["clang", "-O2", "-Wall", "-I", os.path.join(ROOT, "app"), "-I", gen, os.path.join(HERE, "player_test.c"),
                os.path.join(ROOT, "app", "player.c"), os.path.join(ROOT, "app", "world.c"), os.path.join(gen, "tables.c"),
                "-o", exe], check=True)
b, hm = W.generate(2)
rng = random.Random(7)
env = dict(os.environ)
if a.props:
    for _ in range(3000):
        x, z = rng.randrange(1, W.WX - 1), rng.randrange(1, W.WX - 1)
        y = V.ground(b, x, z)
        if y < W.WY - 3:
            t = rng.choice((W.FENCE, W.FENCE, W.LADDER, W.LAVA))
            for k in range(rng.randrange(1, 3)):
                b[W.idx(x, z, y + k)] = t
    wf = os.path.join(out, "world_props.bin")
    open(wf, "wb").write(bytes(b))
    env["BW_WORLD"] = wf
lines, ref = [], []
for k in range(a.n):
    x, z = rng.randrange(4, W.WX - 4), rng.randrange(4, W.WX - 4)
    p = PL.Player((x << 8) + rng.randrange(256), V.ground(b, x, z) << 8, (z << 8) + rng.randrange(256), rng.randrange(1024))
    lines.append(f"P {p.x} {p.y} {p.z} {p.yaw}")
    fw = st = j = 0
    for i in range(a.steps):
        if rng.random() < 0.1:
            fw, st, j = rng.choice((-1, 0, 1, 1, 1)), rng.choice((-1, 0, 0, 1)), int(rng.random() < 0.15)
        dy, dp = rng.choice((0, 0, 0, 9, -9, 40)), rng.choice((0, 0, 7, -7))
        lines.append(f"S {fw} {st} {j} {dy} {dp}")
        p.tick(b, fw, st, j, dy, dp)
        ref.append(f"{p.x} {p.y} {p.z} {p.vy} {p.yaw} {p.pitch} {int(p.ground)} {p.autojumps}")
sf = os.path.join(out, "player_script.txt")
open(sf, "w").write("\n".join(lines) + "\n")
got = subprocess.run([exe, "2", sf], capture_output=True, text=True, check=True, env=env).stdout.split("\n")
bad = sum(1 for r, g in zip(ref, got) if r != g)
first = next((i for i, (r, g) in enumerate(zip(ref, got)) if r != g), None)
if first is not None:
    print("first difference at step", first, "\n  py", ref[first], "\n  c ", got[first])
print(f"{len(ref) - bad}/{len(ref)} steps identical; auto-jumps {ref[-1].split()[-1]} in the last script")

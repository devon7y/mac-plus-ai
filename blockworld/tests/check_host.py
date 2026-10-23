#!/usr/bin/env python3
"""Build the host harness and check the C renderer and generator against the Python reference.
    check_host.py [--seed 2] [--n 40] [--nc 96 --h 288 --dmax 16]"""
import argparse, os, random, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "engine")); sys.path.insert(0, os.path.join(ROOT, "proto"))
import numpy as np
import world as W, render as R, views as V

ap = argparse.ArgumentParser()
ap.add_argument("--seed", type=int, default=2)
ap.add_argument("--n", type=int, default=40)
ap.add_argument("--nc", type=int, default=96)
ap.add_argument("--h", type=int, default=288)
ap.add_argument("--dmax", type=int, default=16)
ap.add_argument("--synthetic", action="store_true", help="stress world: floating blocks, overhangs, pillars")
ap.add_argument("--model", action="store_true", help="render with tests/cast_model.c (the 68000 caster's logic)")
ap.add_argument("--moving", action="store_true", help="moving frames: even columns, 8 px wide")
ap.add_argument("--wide", action="store_true", help="Full size moving frames: columns 0, 4, ..., 16 px wide")
ap.add_argument("--shade", default="0,0,0,0", help="dither per light class (0 sky, 1 tree, 2 covered, 3 torch)")
ap.add_argument("--lit", action="store_true", help="random torch-lit block ranges in some columns")
ap.add_argument("--pitch", type=int, default=0, help="every camera's pitch random in +-PITCH (default: the mix)")
a = ap.parse_args()
out = os.path.join(ROOT, "build", "host")
os.makedirs(out, exist_ok=True)
gen = os.path.join(ROOT, "build", "gen")
subprocess.run([sys.executable, os.path.join(ROOT, "app", "gen_tables.py"), gen], check=True)
exe = os.path.join(out, "host_render")
extra = ["-DCAST_MODEL", os.path.join(HERE, "cast_model.c")] if a.model else []
subprocess.run(["clang", "-O2", "-g", "-Wall", "-Wno-unused-function", "-I", os.path.join(ROOT, "app"), "-I", gen,
                os.path.join(HERE, "host_render.c"), os.path.join(ROOT, "app", "render.c"),
                os.path.join(ROOT, "app", "world.c"), os.path.join(gen, "tables.c"), "-o", exe] + extra, check=True)
b, hm = W.generate(a.seed)
env = dict(os.environ)
if a.model:
    env["BW_ASM"] = "1"
if a.moving:
    env["BW_MOVING"] = "1"
if a.wide:
    env["BW_WIDE"] = "1"
if a.synthetic:                       # random floating blocks and overhangs over flat ground
    import random as _r
    rr = _r.Random(a.seed)
    b = bytearray(W.WX * W.WX * W.WY)
    for x in range(W.WX):
        for z in range(W.WX):
            for y in range(0, 20):
                b[W.idx(x, z, y)] = W.ROCK if y < 17 else (W.SOIL if y < 19 else W.TURF)
            for _ in range(rr.randrange(4)):
                y = rr.randrange(20, W.WY - 2)
                for k in range(rr.randrange(1, 4)):
                    b[W.idx(x, z, min(W.WY - 1, y + k))] = rr.choice((W.ROCK, W.LEAVES, W.TURF, W.LOG, W.GLASS))
    hm = [19] * (W.WX * W.WX)
    wf = os.path.join(out, "world_syn.bin")
    open(wf, "wb").write(bytes(b))
    env["BW_WORLD"] = wf
shade = tuple(int(v) for v in a.shade.split(","))
env["BW_SHADE"] = a.shade
segs = W.all_segments(b)
if a.lit:                             # columns lit over [lo, hi): their overlapping segments class 3
    rl = random.Random(a.seed * 11 + 5)
    lit = bytearray(W.WX * W.WX * 2)
    for i in range(len(segs)):
        if rl.randrange(6) == 0:
            lo = rl.randrange(W.WY - 1)
            hi = min(W.WY, lo + 1 + rl.randrange(8))
            lit[2 * i], lit[2 * i + 1] = lo, hi
            segs[i] = [(t, bo, ty, f, 3 if bo < hi and t > lo and (t <= hi or not f & W.TOPEXP) else c)
                       for t, bo, ty, f, c in segs[i]]     # (as world.c light_col)
    lf = os.path.join(out, "lit.bin")
    open(lf, "wb").write(bytes(lit))
    env["BW_LIT"] = lf
at = R.make_seg_lookup(segs)
farh = W.make_farh(W.coarse_heights(b))
rng = random.Random(a.seed * 7 + 1)
cams = []
while len(cams) < a.n:
    x, z = rng.randrange(2, W.WX - 2), rng.randrange(2, W.WX - 2)
    kind = rng.randrange(4)
    if kind == 0:                         # anywhere in the air or underground, odd angles
        y = rng.randrange(256, W.WY * 256)
    else:                                 # standing on the ground
        y = (V.ground(b, x, z) << 8) + 415
    pitch = rng.randrange(-200, 201) if kind == 1 else rng.randrange(-40, 41)
    if a.pitch:
        pitch = rng.randrange(-a.pitch, a.pitch + 1)
    cams.append(R.Cam((x << 8) + rng.randrange(256), y, (z << 8) + rng.randrange(256), rng.randrange(1024), pitch))
camf = os.path.join(out, "cams.txt")
open(camf, "w").write("".join(f"{c.x} {c.y} {c.z} {c.yaw} {c.pitch}\n" for c in cams))
r = subprocess.run([exe, str(a.seed), str(a.nc), str(a.h), str(a.dmax), camf, out], capture_output=True, text=True, check=True, env=env)
stats = [list(map(int, ln.split()[2::2])) for ln in r.stdout.split("\n") if ln.strip()]
cw = open(os.path.join(out, "world.bin"), "rb").read()
print("world:", "IDENTICAL" if cw == bytes(b) else f"DIFFERENT ({sum(x != y for x, y in zip(cw, b))} bytes)")
cfg = R.Cfg(nc=a.nc, h=a.h, dmax=a.dmax)
bad = 0
for i, cam in enumerate(cams):
    ref = np.packbits(R.render(at, cam, cfg, farh=farh, moving=2 if a.wide else a.moving, shade=shade), axis=1)
    got = np.frombuffer(open(os.path.join(out, f"frame_{i}.bin"), "rb").read(), np.uint8).reshape(a.h, a.nc // 2)
    nd = int(np.count_nonzero(np.unpackbits(ref ^ got)))
    if nd:
        bad += 1
        ys = np.nonzero((ref ^ got).any(axis=1))[0]
        print(f"frame {i}: {nd} pixels differ, rows {ys[:10]} cam {cam.x} {cam.y} {cam.z} {cam.yaw} {cam.pitch}")
st = np.array(stats).mean(axis=0) / a.nc
print(f"{a.n - bad}/{a.n} frames bit-exact ({a.nc}x{a.h}, view {a.dmax}); per column: cells {st[0]:.1f} segs {st[1]:.1f} "
      f"pieces {st[2]:.1f} projections {st[3]:.1f}")
sys.exit(1 if bad or cw != bytes(b) else 0)

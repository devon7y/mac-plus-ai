#!/usr/bin/env python3
"""Scripted walk with the player physics and the reference renderer (M0 controls demo).
    walk.py [--seed 4] [--out build/m0/walk]
Inputs per tick (20/s): W/S forward, A/D strafe, mouse turn, Space jump; auto-jump climbs steps.
Writes a frame every 3 ticks (about the planned frame rate), a contact sheet and a GIF."""
import argparse, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "engine")); sys.path.insert(0, HERE)
from PIL import Image, ImageDraw
import world as W, render as R, player as PL, views as V
PL.SIN, PL.COS = R.SIN, R.COS

ap = argparse.ArgumentParser()
ap.add_argument("--seed", type=int, default=4)
ap.add_argument("--out", default=os.path.join(HERE, "..", "build", "m0", "walk"))
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
b, hm = W.generate(a.seed)
at = R.make_seg_lookup(W.all_segments(b))
cfg = R.Cfg()
# start at a scenic spot (proto/scenic.py, seed 8 view 4) unless --seed is changed
sx, sz, yaw0 = (20, 39, 689) if a.seed == 8 else (32, 32, 0)
p = PL.Player((sx << 8) + 128, V.ground(b, sx, sz) << 8, (sz << 8) + 128, yaw=yaw0)
p.pitch = -15
# autopilot: walk forward with gentle mouse turns, look around; a wall that auto-jump can't
# climb makes it turn (a scripted stand-in for the player's keys and mouse)
frames, log = [], []
stuck = 0
for t in range(1, 241):
    fwd, strafe, jump, dyaw, dpitch = 1, 0, 0, 0, 0
    if 60 <= t < 80:
        dyaw = 10                                     # mouse turn right
    if 120 <= t < 130:
        fwd, strafe = 1, -1                           # W + A
    if 150 <= t < 165:
        dpitch = 4                                    # look up
    if 200 <= t < 215:
        dpitch = -6                                   # look down
    if t in (100, 101):
        jump = 1                                      # Space
    if stuck > 3:
        dyaw = 48
    ox, oz = p.x, p.z
    p.tick(b, fwd, strafe, jump, dyaw, dpitch)
    stuck = stuck + 1 if (p.x, p.z) == (ox, oz) and p.ground else 0
    if t % 3 == 0:
        im = V.to_img(R.render(at, p.cam(R), cfg))
        frames.append(im)
        log.append(f"t={t:3d} x={p.x / 256:6.2f} y={p.y / 256:6.2f} z={p.z / 256:6.2f} yaw={p.yaw:4d} "
                   f"pitch={p.pitch:4d} ground={int(p.ground)} autojumps={p.autojumps}")
print("\n".join(log[::4]))
print("frames", len(frames), "autojumps", p.autojumps)
frames[0].save(os.path.join(a.out, "walk.gif"), save_all=True, append_images=frames[1:], duration=150, loop=0)
pick = frames[::max(1, len(frames) // 12)][:12]
sheet = Image.new("L", (3 * 392, 4 * 296), 255)
for i, im in enumerate(pick):
    sheet.paste(im, ((i % 3) * 392, (i // 3) * 296))
sheet.save(os.path.join(a.out, "walk_sheet.png"))

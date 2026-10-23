#!/usr/bin/env python3
"""Compare the check frames dumped from MAME (RUN_DIR/check_<cfg>_<m|s>.bin, column-pair buffers)
with engine/render.py: m = moving (even columns, 8 px wide), s = still (all columns, 4 px).
    verify_m1.py RUN_DIR"""
import glob, json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "engine"))
import numpy as np
import render as R, world as W
CFGS = [(80, 240, 12), (72, 216, 12), (64, 192, 12), (80, 240, 10), (72, 216, 10), (64, 192, 10)]
cams = json.load(open(os.path.join(ROOT, "build", "gen", "bench_cams.json")))
b, hm = W.generate(2)
at = R.make_seg_lookup(W.all_segments(b))
farh = W.make_farh(W.coarse_heights(b))
cam = R.Cam(*cams[0])


def rows(buf, nc, h):
    """column pairs -> plain rows: byte j of row y is at (j >> 1) * 2h + 2y + (j & 1)"""
    a = np.frombuffer(buf, np.uint8)[: nc // 2 * h].reshape(nc // 4, h, 2)
    return a.transpose(1, 0, 2).reshape(h, nc // 2)


ok = True
for fn in sorted(glob.glob(os.path.join(sys.argv[1], "check_*_*.bin"))):
    ci, tag = os.path.basename(fn)[6:-4].split("_")
    ci = int(ci)
    nc, h, dmax = CFGS[ci]
    ref = np.packbits(R.render(at, cam, R.Cfg(nc=nc, h=h, dmax=dmax), farh=farh, moving=tag == "m"), axis=1)
    got = rows(open(fn, "rb").read(), nc, h)
    nd = int(np.count_nonzero(np.unpackbits(ref ^ got)))
    ok &= nd == 0
    print(f"cfg {ci} {'moving' if tag == 'm' else 'still '} ({nc * 4}x{h}, view {dmax}): " +
          ("BIT-EXACT" if nd == 0 else f"{nd} pixels differ"))
sys.exit(0 if ok else 1)

#!/usr/bin/env python3
"""Compare the M1 check frame dumped from MAME with engine/road.py (bit-exact).
    verify_m1.py RUN_DIR [--w 384 --h 240 --hy 80]"""
import argparse
import os
import sys
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))
import m1common as M  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("run")
ap.add_argument("--w", type=int, default=384)
ap.add_argument("--h", type=int, default=240)
ap.add_argument("--hy", type=int, default=80)
a = ap.parse_args()
cfg = M.road.RoadConfig(W=a.w, H=a.h, HY=a.hy)
tabs = M.road.build_tables(cfg)
cx, ph = M.road.geometry(cfg, tabs, M.road.Track(M.TRACK), M.CHK_Z, M.CHK_X)
ref = M.road.render(cfg, tabs, cx, ph, M.stages.pattern_sets(M.stages.STAGES[M.STAGE]), M.sky_rows(cfg))
got = np.unpackbits(np.frombuffer(open(os.path.join(a.run, "check.bin"), "rb").read(), np.uint8)).reshape(cfg.H, cfg.W)
bad = np.argwhere(ref != got)
print(f"{cfg.W}x{cfg.H}: {len(bad)} differing pixels" + ("  BIT-EXACT" if not len(bad) else ""))
if len(bad):
    rows = sorted(set(bad[:, 0]))
    print("rows:", rows[:20])
    y = rows[0]
    print("ref", "".join(map(str, ref[y])))
    print("got", "".join(map(str, got[y])))

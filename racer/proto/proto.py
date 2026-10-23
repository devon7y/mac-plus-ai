#!/usr/bin/env python3
"""Look prototype: renders Endurance frames as PNGs (2x) with the reference road model.

Usage: proto.py OUTDIR
"""
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "engine"))
sys.path.insert(0, os.path.join(HERE, ".."))
import road  # noqa: E402
from art import stages, scenery  # noqa: E402


def save(img, path, scale=2):
    im = Image.fromarray(((1 - img) * 255).astype(np.uint8), "L")
    im = im.resize((img.shape[1] * scale, img.shape[0] * scale), Image.NEAREST)
    im.save(path)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    cfg = road.RoadConfig()
    tabs = road.build_tables(cfg)
    C = 1 << 16
    track = road.Track([(2000, 0), (3000, int(0.006 * C)), (1500, 0), (3000, int(-0.009 * C)), (4000, 0)])
    mtn = scenery.mountains(512, 30)
    shots = [("straight", 100, 0, "morning"), ("curve_in", 1300, 0, "morning"),
             ("curve_full", 2600, 30, "morning"), ("left", 6000, -40, "morning"),
             ("snow", 2600, 0, "snow"), ("dusk", 6000, 0, "dusk"), ("night", 2600, 0, "night"),
             ("fog", 6000, 0, "fog"), ("dawn", 100, 0, "dawn")]
    for name, camz, camx, st in shots:
        cx, ph = road.geometry(cfg, tabs, track, camz, camx)
        S = stages.STAGES[st]
        sky = scenery.sky(cfg, S, mtn, scroll=camz // 20)
        img = road.render(cfg, tabs, cx, ph, stages.pattern_sets(S), sky)
        if S.get("fog_k"):
            scenery.apply_fog(img, cfg, S)
        save(img, os.path.join(out, f"{name}.png"))
    print("wrote", out)


if __name__ == "__main__":
    main()

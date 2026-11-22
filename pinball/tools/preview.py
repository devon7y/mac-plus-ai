#!/usr/bin/env python3
"""Debug picture of the table geometry: walls (black), ramp layer (blue), sensors (green),
discs (cyan), flippers at rest and up (grey/red). python3 -I preview.py out.png [scale]"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image, ImageDraw  # noqa: E402

import table as T  # noqa: E402


def main():
    out = sys.argv[1]
    k = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    T.build()
    im = Image.new("RGB", (T.W * k, T.H * k), "white")
    d = ImageDraw.Draw(im)
    S = lambda p: (p[0] * k, p[1] * k)
    for s in T.SHAPES:
        col = (0, 0, 0) if s["layers"] & 1 else (40, 90, 230)
        if s["kind"] == "SEG":
            w = max(1, int(round((2 * s["rad"] + 1) * k)))
            d.line([S(s["a"]), S(s["b"])], fill=col, width=w)
        elif s["kind"] == "CIRCLE":
            c, r = s["c"], s["r"]
            d.ellipse([S((c[0] - r, c[1] - r)), S((c[0] + r, c[1] + r))], outline=col, width=2)
        elif s["kind"] == "ARC":
            c, r = s["c"], s["r"]
            d.arc([S((c[0] - r, c[1] - r)), S((c[0] + r, c[1] + r))], s["a0"], s["a1"], fill=col, width=2)
        elif s["kind"] == "LINE":
            col = (240, 140, 0) if "LAYER" in s["flags"] else (0, 170, 0)
            d.line([S(s["a"]), S(s["b"])], fill=col, width=1)
        elif s["kind"] == "DISC":
            c, r = s["c"], s["r"]
            d.ellipse([S((c[0] - r, c[1] - r)), S((c[0] + r, c[1] + r))], outline=(0, 190, 210))
    for f in T.flippers():
        for ang, col in ((f["rest"], (150, 150, 150)), (f["up"], (220, 0, 0))):
            a = math.radians(ang)
            p = f["pivot"]
            q = (p[0] + T.FLIP_LEN * math.cos(a), p[1] + T.FLIP_LEN * math.sin(a))
            d.line([S(p), S(q)], fill=col, width=int(4 * k))
    # a ball for scale
    d.ellipse([S((T.SHOOT_X0 + 3, T.PLUNGER_Y - 8)), S((T.SHOOT_X0 + 11, T.PLUNGER_Y))], outline=(200, 0, 200), width=2)
    d.line([S((T.SHOOT_X0, T.PLUNGER_Y)), S((T.SHOOT_X1, T.PLUNGER_Y))], fill=(200, 0, 200), width=2)
    im.save(out)
    print(out)


if __name__ == "__main__":
    main()

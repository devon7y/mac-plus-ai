#!/usr/bin/env python3
"""Plot a ball path dumped by tests/test_phys --dump NAME (or the game simulator) as a PNG.

    test_phys --dump cradle | python3 -I plot_path.py out.png [--scale 2]

Input lines: 'S kind x0 y0 x1 y1 r' shapes (px), 'L x0 y0 x1 y1' flipper lines, then one line per
step: 'i x y ...' ball positions and 'Fk angle' flipper angles. Draws the shapes in grey, every
ball position as a dot (darker = later), and the flippers at the first and last step.
"""
import math
import sys

from PIL import Image, ImageDraw


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "path.png"
    scale = int(sys.argv[sys.argv.index("--scale") + 1]) if "--scale" in sys.argv else 2
    shapes, flips, steps = [], {}, []
    for line in sys.stdin:
        t = line.split()
        if not t or t[0] in ("PASS", "FAIL", "ALL", "FAILED:"):
            continue
        if t[0] == "S":
            shapes.append((int(t[1]),) + tuple(float(v) for v in t[2:]))
        elif t[0] == "FD":
            flips[int(t[1])] = tuple(float(v) for v in t[2:])
        else:
            balls, fl, i = [], {}, 0
            while i < len(t):
                if t[i].startswith("F"):
                    fl[int(t[i][1:])] = int(t[i + 1])
                    i += 2
                else:
                    balls.append((float(t[i + 1]), float(t[i + 2])))
                    i += 3
            steps.append((balls, fl))
    w, h = 256 * scale, 330 * scale
    im = Image.new("RGB", (w, h), "white")
    d = ImageDraw.Draw(im)
    S = lambda v: v * scale
    for s in shapes:
        kind = s[0]
        if kind in (1, 4):  # segment / sensor line
            d.line([S(s[1]), S(s[2]), S(s[3]), S(s[4])], fill=(120, 120, 120) if kind == 1 else (120, 200, 120), width=1)
        elif kind in (2, 5):
            r = s[5]
            d.ellipse([S(s[1] - r), S(s[2] - r), S(s[1] + r), S(s[2] + r)], outline=(120, 120, 120) if kind == 2 else (120, 160, 220))
        elif kind == 3:
            r, a0, a1 = s[5], s[6], s[7]
            d.arc([S(s[1] - r), S(s[2] - r), S(s[1] + r), S(s[2] + r)], a0, a1, fill=(120, 120, 120))
    n = len(steps)
    for k, (balls, fl) in enumerate(steps):
        c = int(200 - 200 * k / max(1, n - 1))
        for (x, y) in balls:
            d.point((S(x), S(y)), fill=(255, c, c) if k % 50 else (0, 0, 255))
    for k in (0, n - 1):
        if k < 0 or not steps:
            continue
        for fi, ang in steps[k][1].items():
            if fi not in flips:
                continue
            px, py, ln = flips[fi]
            a = ang * 2 * math.pi / 65536
            d.line([S(px), S(py), S(px + ln * math.cos(a)), S(py + ln * math.sin(a))], fill=(0, 0, 0) if k else (180, 180, 180), width=3)
    if steps:
        for (x, y) in steps[-1][0]:
            d.ellipse([S(x - 4), S(y - 4), S(x + 4), S(y + 4)], outline=(0, 0, 0))
    im.save(out)
    print(f"{out}: {n} steps, {len(shapes)} shapes")


if __name__ == "__main__":
    main()

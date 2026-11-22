#!/usr/bin/env python3
"""Draw Orbit Duel's 32x32 Finder icon as text for toolchain/icons/add_icon.py.

    make_icon.py <out.txt>

A black disc of space with the glowing star in the middle, a dotted orbit, and the two ships
(Needle upper left, Wedge lower right) in white. X = black, o = white inside the icon, . = outside.
"""
import math
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_art  # noqa: E402

N = 32
g = [['.'] * N for _ in range(N)]
for r in range(N):
    for c in range(N):
        if math.hypot(c + 0.5 - 16, r + 0.5 - 16) <= 15.6:
            g[r][c] = 'X'


def white(c, r):
    if 0 <= c < N and 0 <= r < N and g[r][c] == 'X':
        g[r][c] = 'o'


# dotted orbit (an ellipse, tilted a little)
for k in range(0, 360, 15):
    a = math.radians(k)
    if abs(math.cos(a)) > 0.8:
        continue                        # leave room for the ships
    white(int(16 + 10.5 * math.cos(a)), int(16 + 10.5 * math.sin(a)))
# the star: a bright core and rays
for dc, dr in [(0, 0), (-1, 0), (0, -1), (-1, -1), (1, 0), (0, 1), (-2, 0), (1, -1), (-1, 1), (-2, -1), (0, -2), (-1, -2),
               (0, 1), (-1, 1), (1, -1), (-3, -1), (2, 0), (0, 2), (-1, -3), (-3, 2), (2, -3), (2, 2), (-3, -3)]:
    white(15 + dc, 15 + dr)
# the ships, from the game's own shapes
for shape, cx, cy, ang in [(gen_art.NEEDLE, 8.5, 10.5, 0.0), (gen_art.WEDGE, 23.5, 21.5, math.pi)]:
    pts = gen_art.rot_screen([(x * 0.8, y * 0.8) for x, y in shape], ang, cx, cy)
    segs = [(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts))]
    for r in range(N):
        for c in range(N):
            if any(gen_art.seg_dist(c + 0.5, r + 0.5, a[0], a[1], b[0], b[1]) <= 0.55 for a, b in segs):
                white(c, r)
out = ["# Orbit Duel (application, APPL/ODul): two ships orbiting a star (made by duel/tools/make_icon.py)",
       "# Format (add_icon.py): X black, . white (transparent where it reaches the edge), o white opaque, _ transparent"]
out += ["".join(row) for row in g]
open(sys.argv[1], "w").write("\n".join(out) + "\n")
print("\n".join(out[2:]))

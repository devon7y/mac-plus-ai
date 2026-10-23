#!/usr/bin/env python3
"""Rail Runner view geometry and the static background (shared by the game and the previews).

Camera: centred on the middle lane at height CAM_H, ZP zu behind the player, looking along +z.
A world point (X, Y, zc) with zc = depth from the camera projects to
    x = CX + F * X / zc          y = HOR + F * (CAM_H - Y) / zc
Every plane the game draws (ground, roofs, ramps) maps each screen row to ONE depth, and every
edge parallel to z is a straight line through the vanishing point (CX, HOR). The renderer uses
that: roofs, sides and ramps are filled row by row with linear edge stepping, no per-pixel maths.

The background (sky, skyline, side walls, gravel, rails) never changes; only the sleepers move.
Each row therefore has two precomputed variants (without / with sleepers); per frame the game
picks one per row from the row's depth and the camera's z.
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "proto"))
import params as P  # noqa: E402

U = P.U
CAM_H = 1638          # camera height (1.6 u)
ZP = 2048             # camera is 2 u behind the player
WALL_X = 1950         # side walls at |X| = 1.9 u
WALL_TOP = 1300       # wall height
POLE_X = 1700         # catenary poles
POLE_TOP = 1520
POLE_ARM_Y = 1470
POLE_ARM_X = 820      # arm reaches in to |X| = 0.8 u
POLE_W = 70
POLE_SPACING = 6 * U
SLEEPER_P = 512       # sleeper period (zu), power of two
SLEEPER_LEN = 150
SLEEPER_HALF = 440    # sleepers span lane centre +- this
RAIL_X = 360          # rails at lane centre +- this
RAIL_W = 34
ZNEAR = 560           # nothing nearer than this to the camera is drawn
SLEEPER_FAR = 9 * U   # rows deeper than this never animate (they would alias)


class Geom:
    def __init__(self, W=384, H=240):
        assert W % 32 == 0
        self.W, self.H = W, H
        self.RB = W // 8
        self.F = round(200 * W / 384)
        self.CX = W // 2
        feet = H - round(18 * H / 240)                   # the player's feet row
        self.HOR = feet - (self.F * CAM_H) // ZP
        self.FEET = self.HOR + (self.F * CAM_H) // ZP
        # depth of the ground at each row (0 above/at the horizon); clamp to 32767
        self.zground = np.zeros(H, np.int32)
        for y in range(self.HOR + 1, H):
            self.zground[y] = min(32767, self.F * CAM_H // (y - self.HOR))

    # projections (integer, as in the game)
    def px(self, X, zc):
        return self.CX + (self.F * X) // zc

    def py(self, Y, zc):
        return self.HOR + (self.F * (CAM_H - Y)) // zc

    def describe(self):
        return (f"{self.W}x{self.H}: F={self.F} HOR={self.HOR} feet row={self.FEET} "
                f"lane={self.F * U // ZP}px at the player")


# ------------------------------------------------------------------ background
def dither(level, y, x):
    """Ordered dither, True = black. level 0..16."""
    b4 = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]])
    return b4[y % 4, x % 4] < level


def background(g):
    """Returns (rows[2][H][W] bool, sleeper_row[H] bool): variant 0 without sleepers, 1 with.
    sleeper_row[y] says whether row y animates."""
    W, H, F, HOR, CX = g.W, g.H, g.F, g.HOR, g.CX
    base = np.zeros((H, W), bool)
    yy, xx = np.mgrid[0:H, 0:W]
    rng = np.random.RandomState(7)

    # --- sky: white, a few soft dither bands near the horizon
    sky = np.zeros((H, W), bool)
    for y in range(0, HOR + 1):
        d = HOR - y
        lvl = 4 if d < 3 else 2 if d < 7 else 1 if d < 12 else 0
        sky[y] = [dither(lvl, y, x) for x in range(W)]

    # --- skyline: buildings standing at the horizon, seen above the walls
    city = np.zeros((H, W), bool)
    cityfill = np.zeros((H, W), bool)
    x = -rng.randint(0, 20)
    while x < W:
        bw = rng.randint(14, 34)
        bh = rng.randint(8, max(9, HOR - 18))
        top = HOR - bh
        x0, x1 = max(x, 0), min(x + bw, W)
        if x1 > x0:
            cityfill[top:H, x0:x1] = True
            body = np.zeros((H - top, x1 - x0), bool)
            for r in range(body.shape[0]):
                for c in range(body.shape[1]):
                    body[r, c] = dither(11, top + r, x0 + c)
            # windows: white dots on a grid
            for r in range(3, body.shape[0] - 2, 4):
                for c in range(2 + (x0 - x) % 2, body.shape[1] - 1, 4):
                    if rng.rand() < 0.75:
                        body[r, c] = False
            body[0, :] = True
            body[:, 0] = True
            body[:, -1] = True
            city[top:H, x0:x1] = body
            if rng.rand() < 0.35 and bw > 18:          # rooftop water tank / antenna
                ax = x0 + bw // 2
                if ax < W:
                    for r in range(top - 5, top):
                        if r >= 0:
                            cityfill[r, ax] = True
                            city[r, ax] = True
        x += bw + rng.randint(-4, 3)

    # --- per-pixel classification below the horizon
    rows0 = np.zeros((H, W), bool)
    rows1 = np.zeros((H, W), bool)
    sleeper_row = np.zeros(H, bool)
    for y in range(H):
        for x in range(W):
            v = sky[y, x]
            if cityfill[y, x]:
                v = city[y, x]
            rows0[y, x] = rows1[y, x] = v
    for y in range(HOR + 1, H):
        zc = F * CAM_H / (y - HOR)
        zrow_near = F * CAM_H / (y - HOR + 0.5)
        zrow_far = F * CAM_H / max(y - HOR - 0.5, 0.25)
        dz = zrow_far - zrow_near
        sleeper_row[y] = zc < SLEEPER_FAR
        for x in range(W):
            X = (x + 0.5 - CX) * zc / F
            ax = abs(X)
            # side walls: vertical planes at |X| = WALL_X
            if ax >= WALL_X:
                zw = F * WALL_X / max(abs(x + 0.5 - CX), 0.5)
                Y = CAM_H - (y - HOR) * zw / F
                if Y > WALL_TOP:
                    continue                                   # sky / city above the wall
                if Y > WALL_TOP - 110:
                    v = False                                  # white coping
                elif Y > WALL_TOP - 150:
                    v = True                                   # shadow under the coping
                elif Y < 70:
                    v = True                                   # dark foot of the wall
                else:
                    v = dither(10, y, x)                       # concrete
                rows0[y, x] = rows1[y, x] = v
                continue
            # ground
            lane = int(np.floor((X + 1.5 * U) / U))
            in_track = 0 <= lane <= 2
            xc = (lane - 1) * U if in_track else 0
            dx = X - xc
            v = dither(2, y, x) if not (in_track and abs(dx) < SLEEPER_HALF) else dither(3, y, x)
            v1 = v
            if in_track and abs(dx) < SLEEPER_HALF:
                if sleeper_row[y]:
                    v1 = dither(13, y, x)                      # sleeper (dark)
                else:
                    v = v1 = dither(6, y, x)                   # far: blended sleepers
            rows0[y, x], rows1[y, x] = v, v1
        # rails: at least one pixel wide, drawn over both variants
        for lane in range(3):
            xc = (lane - 1) * U
            for s in (-1, 1):
                xr = xc + s * RAIL_X
                a = CX + F * (xr - RAIL_W / 2) / zc
                b = CX + F * (xr + RAIL_W / 2) / zc
                x0 = int(np.floor(a))
                x1 = max(x0 + 1, int(np.floor(b)))
                x0, x1 = max(x0, 0), min(x1, W)
                rows0[y, x0:x1] = True
                rows1[y, x0:x1] = True
                # the sleeper variant shows the rail's white top edge for contrast
    # wall/ground boundary line: the foot of the wall is already dark
    return [rows0, rows1], sleeper_row


def pack_rows(bits):
    """bool [H][W] -> bytes, MSB first, row-major."""
    return np.packbits(bits.astype(np.uint8), axis=1).tobytes()


if __name__ == "__main__":
    from PIL import Image
    for W, H in ((384, 240), (320, 200), (256, 160)):
        g = Geom(W, H)
        print(g.describe())
    g = Geom()
    rows, srow = background(g)
    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "art", "preview_bg.png")
    im = np.concatenate([rows[0], np.ones((4, g.W), bool), rows[1]])
    Image.fromarray(np.where(im, 0, 255).astype(np.uint8)).resize((g.W * 2, im.shape[0] * 2), Image.NEAREST).save(out)
    print("->", out)

#!/usr/bin/env python3
"""Reference renderer for Rail Runner (previews, and the model for render.c).

Uses the same primitives and integer maths as the game:
  - background rows: variant per row (sleeper / no sleeper) from the camera z
  - fill_span(y, x0, x1, pat): pattern fill of one row segment (clipped)
  - face(art, x0, y0, x1, y1): a vertical face facing the camera, drawn from a list of
    rectangles in 1/64ths of the face (art.py), each a pattern fill or a framed fill
  - trains: roof (rows of the plane Y = TRAIN_H), inner side face (outer lanes only),
    then the front face; ramps: straight projected edges; poles: vertical spans
  - sprites: masked bitmaps (player, guard, dog, coins, power-ups)

Usage: scene.py demo [seed] [seconds] -> runner/art/preview_scene_*.png
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
RUNNER = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(RUNNER, "proto"))
import geom as G  # noqa: E402
import art  # noqa: E402
import params as P  # noqa: E402

PATS = art.PATTERNS


def pat_row(p, y):
    return PATS[p][y & 7]


class Frame:
    def __init__(self, g, bg_rows, sleeper_row):
        self.g = g
        self.bits = np.zeros((g.H, g.W), bool)
        self.bg = bg_rows
        self.srow = sleeper_row

    def background(self, zcam):
        g = self.g
        for y in range(g.H):
            v = 0
            if self.srow[y]:
                v = 1 if ((zcam + int(g.zground[y])) & (G.SLEEPER_P - 1)) < G.SLEEPER_LEN else 0
            self.bits[y] = self.bg[v][y]

    def span(self, y, x0, x1, p):
        """Fill pixels x0..x1 inclusive of row y with pattern p (clipped)."""
        g = self.g
        if y < 0 or y >= g.H:
            return
        if x0 > x1:
            x0, x1 = x1, x0
        x0, x1 = max(x0, 0), min(x1, g.W - 1)
        if x0 > x1:
            return
        word = pat_row(p, y)
        xs = np.arange(x0, x1 + 1)
        self.bits[y, x0:x1 + 1] = ((word >> (15 - (xs & 15))) & 1).astype(bool)

    def rect(self, x0, y0, x1, y1, p):
        for y in range(max(y0, 0), min(y1, self.g.H - 1) + 1):
            self.span(y, x0, x1, p)

    def face(self, rects, x0, y0, x1, y1):
        """Face covering pixels [x0, x1) x [y0, y1)."""
        w, h = x1 - x0, y1 - y0
        if w <= 0 or h <= 0:
            return
        for kind, u0, v0, u1, v1, p in rects:
            a = x0 + (u0 * w >> 6)
            b = x0 + (u1 * w >> 6) - 1
            c = y0 + (v0 * h >> 6)
            d = y0 + (v1 * h >> 6) - 1
            if b < a or d < c:
                continue
            if kind == art.FRAME:
                self.rect(a, c, b, d, art.BLACK)
                if b - a >= 2 and d - c >= 2:
                    self.rect(a + 1, c + 1, b - 1, d - 1, p)
            else:
                self.rect(a, c, b, d, p)

    def sprite(self, spr, x, y):
        """Draw sprite with its anchor at (x, y)."""
        import sprites as S
        S.composite(self.bits, spr, x - spr.ax, y - spr.ay)


# ------------------------------------------------------------------ object drawing
def draw_block(fr, xc, zn, zf, height, face_art, side_pats, roof_pat, pillars=False):
    """A box on the ground in the lane centred at X = xc, from depth zn to zf (camera-relative),
    TRAIN_W wide and `height` tall: roof, inner side (if visible), then the front face."""
    g = fr.g
    F, HOR, CX = g.F, g.HOR, g.CX
    hw = P.TRAIN_W // 2
    zn_c = max(zn, G.ZNEAR)
    if zf <= zn_c:
        return
    dh = G.CAM_H - height                                   # camera height above the roof
    # roof rows: y = HOR + F*dh/z for z in [zn_c, zf]
    ya = HOR + F * dh // zf
    yb = min(HOR + F * dh // zn_c, g.H - 1)
    xl, xr = xc - hw, xc + hw
    for y in range(max(ya, 0), yb + 1):
        k = y - HOR                                          # x = CX + X*k/dh on the roof
        if k <= 0:
            continue
        x0 = CX + xl * k // dh
        x1 = CX + xr * k // dh
        fr.span(y, x0, x1, roof_pat)
        fr.span(y, x0, x0, art.BLACK)
        fr.span(y, x1, x1, art.BLACK)
    fr.span(ya, CX + xl * (ya - HOR) // dh, CX + xr * (ya - HOR) // dh, art.BLACK)
    # inner side face (outer lanes): plane X = xs, rows between the roof line and ground line
    if xc != 0:
        xs = xr if xc < 0 else xl
        xn = CX + F * xs // zn_c
        xfar = CX + F * xs // zf
        y_top = HOR + F * dh // zf
        y_bot = min(HOR + F * G.CAM_H // zn_c, g.H - 1)
        yroof_n = HOR + F * dh // zn_c                       # roof edge row at the near end
        yground_f = HOR + F * G.CAM_H // zf                  # ground row at the far end
        bands = side_pats                                    # [(Y_from, Y_to, pat)], top to bottom
        for y in range(max(y_top, 0), y_bot + 1):
            k = y - HOR
            if k <= 0:
                continue
            # clamp each line x(Yplane) = CX + xs*k/(CAM_H - Yplane) to [near, far]
            def lx(Yp):
                x = CX + xs * k // (G.CAM_H - Yp)
                lo, hi = (xn, xfar) if xn < xfar else (xfar, xn)
                return min(max(x, lo), hi)
            for (ytop_w, ybot_w, p) in bands:
                a, b = lx(ytop_w), lx(ybot_w)
                if a != b:
                    fr.span(y, a, b, p)
            # outline: top edge (roof line) and the far edge
            fr.span(y, lx(height), lx(height), art.BLACK)
            fr.span(y, xfar, xfar, art.BLACK)
        if pillars:
            z = zn + P.U
            while z < zf:
                if z > zn_c:
                    x = CX + F * xs // z
                    for y in range(HOR + F * (G.CAM_H - 820) // z, HOR + F * (G.CAM_H - 500) // z + 1):
                        fr.span(y, x, x, art.WHITE)
                z += P.U // 2
    # front face
    if zn >= G.ZNEAR:
        x0 = CX + F * xl // zn
        x1 = CX + F * xr // zn
        y0 = HOR + F * dh // zn
        y1 = HOR + F * G.CAM_H // zn
        fr.face(face_art, x0, y0, x1 + 1, y1 + 1)


def draw_ramp(fr, xc, zn, zf):
    g = fr.g
    F, HOR, CX = g.F, g.HOR, g.CX
    hw = P.TRAIN_W // 2
    if zf <= G.ZNEAR:
        return
    zn_c = max(zn, G.ZNEAR)
    # height at the clipped near end
    yn_w = P.TRAIN_H * (zn_c - zn) // (zf - zn)
    # screen endpoints: near (ground or clipped) and far (roof height)
    yN = HOR + F * (G.CAM_H - yn_w) // zn_c
    yF = HOR + F * (G.CAM_H - P.TRAIN_H) // zf
    xlN, xrN = CX + F * (xc - hw) // zn_c, CX + F * (xc + hw) // zn_c
    xlF, xrF = CX + F * (xc - hw) // zf, CX + F * (xc + hw) // zf
    if yN <= yF:
        return
    for y in range(max(yF, 0), min(yN, g.H - 1) + 1):
        t = (y - yF) * 256 // (yN - yF)
        x0 = xlF + ((xlN - xlF) * t >> 8)
        x1 = xrF + ((xrN - xrF) * t >> 8)
        fr.span(y, x0, x1, art.RAMP_PAT)
        fr.span(y, x0, x0, art.BLACK)
        fr.span(y, x1, x1, art.BLACK)
    # cross bars every 0.5 u
    z = zn + P.U // 2
    while z < zf:
        if z > zn_c:
            Y = P.TRAIN_H * (z - zn) // (zf - zn)
            y = HOR + F * (G.CAM_H - Y) // z
            fr.span(y, CX + F * (xc - hw) // z, CX + F * (xc + hw) // z, art.BLACK)
        z += P.U // 2


def draw_face_obj(fr, xc, z, width, height, face_art, y_bottom=0):
    g = fr.g
    if z < G.ZNEAR:
        return
    F, HOR, CX = g.F, g.HOR, g.CX
    x0 = CX + F * (xc - width // 2) // z
    x1 = CX + F * (xc + width // 2) // z
    y0 = HOR + F * (G.CAM_H - y_bottom - height) // z
    y1 = HOR + F * (G.CAM_H - y_bottom) // z
    fr.face(face_art, x0, y0, x1 + 1, y1 + 1)


def draw_poles(fr, zcam):
    g = fr.g
    F, HOR, CX = g.F, g.HOR, g.CX
    z0 = (zcam // G.POLE_SPACING + 1) * G.POLE_SPACING
    zs = []
    z = z0
    while z - zcam < P.DRAW_DIST + G.ZP:
        zs.append(z - zcam)
        z += G.POLE_SPACING
    for zc in reversed(zs):
        if zc < G.ZNEAR:
            continue
        w = max(1, F * G.POLE_W // zc)
        ytop = HOR + F * (G.CAM_H - G.POLE_TOP) // zc
        ybot = HOR + F * G.CAM_H // zc
        yarm = HOR + F * (G.CAM_H - G.POLE_ARM_Y) // zc
        th = max(1, F * 40 // zc)
        for s in (-1, 1):
            x = CX + s * F * G.POLE_X // zc
            for y in range(max(ytop, 0), min(ybot, g.H - 1) + 1):
                fr.span(y, x - w // 2, x - w // 2 + w - 1, art.BLACK)
            xa = CX + s * F * G.POLE_ARM_X // zc
            for y in range(yarm, yarm + th):
                fr.span(y, x, xa, art.BLACK)


# ------------------------------------------------------------------ demo
def demo(seed=3, seconds=8, every=0.5, W=384, H=240):
    from PIL import Image
    import sim
    import sprites as S
    g = G.Geom(W, H)
    rows, srow = G.background(g)
    pats = sim.load_patterns()
    rng = sim.Rng(seed)
    ch = sim.Chainer(pats, rng)
    objs = []
    z = 40 * P.SPEED_STEP_ZU
    nxt = z + 6 * P.U
    spr = S.load() if os.path.exists(os.path.join(RUNNER, "art", "sprites.txt")) else {}
    outs = []
    n = int(seconds * 60)
    for t in range(n):
        v = P.speed_at(z)
        z += v
        while z + P.DRAW_DIST >= nxt:
            i = ch.pick(v)
            objs.extend(sim.place(pats[i], nxt, z, v))
            nxt += (pats[i].rows + P.gap_rows(v)) * P.U
        sim.move_world(objs)
        objs = [o for o in objs if o.z1 > z - 3 * P.U]
        if t % int(every * 60) == 0:
            fr = Frame(g, rows, srow)
            zcam = z - G.ZP
            fr.background(zcam)
            draw_poles(fr, zcam)
            for o in sorted(objs, key=lambda o: -(o.z0 - zcam)):
                xc = (o.lane - 1) * P.U
                zn, zf = o.z0 - zcam, o.z1 - zcam
                if zn > P.DRAW_DIST + G.ZP or zf < G.ZNEAR:
                    continue
                if o.kind == sim.TRAIN:
                    draw_block(fr, xc, zn, zf, P.TRAIN_H, art.TRAIN_FRONT, art.TRAIN_SIDE, art.ROOF_PAT, True)
                elif o.kind == sim.ONCOMING:
                    draw_block(fr, xc, zn, zf, P.TRAIN_H, art.ONCOMING_FRONT, art.ONCOMING_SIDE, art.ROOF_PAT, True)
                elif o.kind == sim.WALL:
                    draw_block(fr, xc, zn, zf, P.TRAIN_H, art.WALL_FRONT, art.WALL_SIDE, art.WALL_TOP)
                elif o.kind == sim.RAMP:
                    draw_ramp(fr, xc, zn, zf)
                elif o.kind == sim.LOW:
                    draw_face_obj(fr, xc, zn, art.BARRIER_W, P.LOW_H, art.LOW_FACE)
                elif o.kind == sim.HIGH:
                    draw_face_obj(fr, xc, zn, art.BARRIER_W, P.HIGH_T, art.HIGH_FACE)
            name = f"rex_run{(t // 6) % 4}"
            if name not in spr:
                name = "rex_run0"
            if name in spr:
                fr.sprite(spr[name], g.CX, g.FEET)
            outs.append(fr.bits.copy())
    sheet = []
    for i in range(0, len(outs), 2):
        pair = outs[i:i + 2]
        if len(pair) == 1:
            pair.append(np.zeros_like(pair[0]))
        sheet.append(np.concatenate([pair[0], np.ones((g.H, 4), bool), pair[1]], axis=1))
        sheet.append(np.ones((4, sheet[-1].shape[1]), bool))
    img = np.concatenate(sheet)
    out = os.path.join(RUNNER, "art", f"preview_scene_{W}.png")
    Image.fromarray(np.where(img, 0, 255).astype(np.uint8)).save(out)
    first = os.path.join(RUNNER, "art", f"preview_frame_{W}.png")
    big = Image.fromarray(np.where(outs[len(outs) // 2], 0, 255).astype(np.uint8))
    big.resize((W * 2, H * 2), Image.NEAREST).save(first)
    print("->", out, first)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "demo":
        demo(*(int(a) for a in sys.argv[2:4]))
    else:
        print(__doc__)

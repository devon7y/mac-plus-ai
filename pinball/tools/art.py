#!/usr/bin/env python3
"""Neural Pinball art: the 1-bit playfield, lamps, sprites and side panel, drawn from the same
numbers as the physics (table.py). Used by gen.py (emit()) and runnable on its own for previews:

    python3 -I art.py OUTDIR      -> OUTDIR/table_art.png (2x), table_lit.png, panel.png, sprites.png

Bitmaps are numpy arrays [y][x] of 0 (white) / 1 (black). Lamps are XOR patches on byte-aligned
rectangles, so the game can switch them in any order.
"""
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import font57  # noqa: E402
import table as T  # noqa: E402

W, H = T.W, T.H
PANEL_W = 192

# ------------------------------------------------------------------------------ patterns
def pat(rows):
    return np.array([[int(c) for c in r] for r in rows], dtype=np.uint8)


P_GRAY25 = pat(["10001000", "00000000", "00100010", "00000000"] * 2)
P_GRAY50 = pat(["10101010", "01010101"] * 4)
P_GRAY12 = pat(["10000000", "00000000", "00001000", "00000000"] * 2)
P_DOTS = pat(["10000000", "00000000", "00000000", "00000000", "00001000", "00000000", "00000000", "00000000"])
P_DIAG = pat(["11000001", "10000011", "00000111", "00001110", "00011100", "00111000", "01110000", "11100000"])
P_CABINET = pat(["11101110", "11011101", "10111011", "01110111"] * 2)
P_BRICK = pat(["11111111", "10000000", "10000000", "10000000", "11111111", "00001000", "00001000", "00001000"])


def tile(p, h, w):
    return np.tile(p, (h // p.shape[0] + 1, w // p.shape[1] + 1))[:h, :w]


# ------------------------------------------------------------------------------ raster helpers
class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.a = np.zeros((h, w), dtype=np.uint8)
        yy, xx = np.mgrid[0:h, 0:w]
        self.px = xx + 0.5
        self.py = yy + 0.5

    def seg_dist(self, a, b):
        ax, ay = a
        bx, by = b
        dx, dy = bx - ax, by - ay
        l2 = dx * dx + dy * dy or 1e-9
        t = np.clip(((self.px - ax) * dx + (self.py - ay) * dy) / l2, 0, 1)
        return np.hypot(self.px - (ax + t * dx), self.py - (ay + t * dy))

    def line(self, a, b, width=1.0, ink=1):
        self.a[self.seg_dist(a, b) <= width / 2 + 0.01] = ink

    def disc(self, c, r, ink=1):
        self.a[np.hypot(self.px - c[0], self.py - c[1]) <= r + 0.01] = ink

    def ring(self, c, r_out, r_in, ink=1):
        d = np.hypot(self.px - c[0], self.py - c[1])
        self.a[(d <= r_out + 0.01) & (d > r_in)] = ink

    def circle_dotted(self, c, r, every=2):
        d = np.hypot(self.px - c[0], self.py - c[1])
        ang = np.degrees(np.arctan2(self.py - c[1], self.px - c[0]))
        m = (np.abs(d - r) <= 0.5) & ((np.floor((ang + 180) * r / 57.3 / every) % 2) == 0)
        self.a[m] = 1

    def arc(self, c, r, a0, a1, width=1.0, ink=1):
        d = np.hypot(self.px - c[0], self.py - c[1])
        ang = np.degrees(np.arctan2(self.py - c[1], self.px - c[0])) % 360
        lo, hi = a0 % 360, a1 % 360
        inside = (ang >= lo) & (ang <= hi) if lo <= hi else (ang >= lo) | (ang <= hi)
        if a1 - a0 >= 360:
            inside = np.ones_like(d, dtype=bool)
        self.a[inside & (np.abs(d - r) <= width / 2 + 0.01)] = ink

    def polygon_mask(self, pts):
        m = np.zeros((self.h, self.w), dtype=bool)
        n = len(pts)
        for i in range(n):
            x0, y0 = pts[i]
            x1, y1 = pts[(i + 1) % n]
            cond = ((y0 <= self.py) & (self.py < y1)) | ((y1 <= self.py) & (self.py < y0))
            with np.errstate(divide="ignore", invalid="ignore"):
                xint = x0 + (self.py - y0) * (x1 - x0) / (y1 - y0)
            m ^= cond & (self.px < xint)
        return m

    def fill(self, mask, p=None, ink=1):
        if p is None:
            self.a[mask] = ink
        else:
            t = tile(p, self.h, self.w)
            self.a[mask] = t[mask]

    def rect(self, x0, y0, x1, y1, ink=1):
        self.a[max(0, y0):max(0, y1), max(0, x0):max(0, x1)] = ink

    def frame(self, x0, y0, x1, y1, ink=1):
        self.rect(x0, y0, x1, y0 + 1, ink)
        self.rect(x0, y1 - 1, x1, y1, ink)
        self.rect(x0, y0, x0 + 1, y1, ink)
        self.rect(x1 - 1, y0, x1, y1, ink)

    def text(self, x, y, s, ink=1, scale=1):
        for i, ch in enumerate(s):
            g = font57.GLYPHS.get(ch) or font57.GLYPHS.get(ch.upper()) or font57.GLYPHS["?"]
            for r in range(7):
                for c in range(5):
                    if g[r][c] == "#":
                        xx, yy = x + (i * 6 + c) * scale, y + r * scale
                        self.a[yy:yy + scale, xx:xx + scale] = ink
        return len(s) * 6 * scale - scale

    def text_c(self, cx, y, s, ink=1, scale=1):
        w = len(s) * 6 * scale - scale
        self.text(int(round(cx - w / 2)), y, s, ink, scale)

    def vtext(self, x, y, s, ink=1):
        for i, ch in enumerate(s):
            self.text(x, y + i * 8, ch, ink)


def wall_side_fill(c):
    """fill everything outside the playfield boundary with the cabinet pattern"""
    inside = np.zeros((H, W), dtype=bool)
    # the playfield region bounded by the outer walls (and the top arcs)
    ax, ay, r = T.LEFT_X + T.ARC_R, T.TOP_Y + T.ARC_R, T.ARC_R
    bx = T.RIGHT_X - T.ARC_R
    px, py = c.px, c.py
    box = (px >= T.LEFT_X) & (px <= T.RIGHT_X) & (py >= T.TOP_Y)
    corner_l = (px < ax) & (py < ay) & (np.hypot(px - ax, py - ay) > r)
    corner_r = (px > bx) & (py < ay) & (np.hypot(px - bx, py - ay) > r)
    inside = box & ~corner_l & ~corner_r
    c.fill(~inside, P_CABINET)
    return inside


# ------------------------------------------------------------------------------ the playfield
LAMPS = []          # (name, rect (x0,y0,x1,y1) in px, draw_on(canvas)) ; exclusive groups share a rect


def lamp(name, rect, draw):
    LAMPS.append((name, rect, draw))


def draw_robot_face(c, cx, cy, state, scale=1):
    """the Turing-test robot: a square head, antenna, two eyes and a mouth.
    state: 'off' (outline), 'on', 'happy', 'suspicious', 'surprised', 'sad'"""
    s = scale
    hw, hh = 11 * s, 9 * s
    x0, y0, x1, y1 = cx - hw, cy - hh, cx + hw + 1, cy + hh + 1
    lit = state != "off"
    fg, bg = (0, 1) if lit else (1, 0)
    c.rect(x0, y0, x1, y1, bg)
    c.frame(x0, y0, x1, y1, 1)
    c.frame(x0 + 1, y0 + 1, x1 - 1, y1 - 1, 1) if not lit else None
    # antenna and ears
    c.rect(cx - 1 * s + (0 if s == 1 else 1), y0 - 4 * s, cx + 1 * s, y0, 1)
    c.disc((cx + 0.5, y0 - 4 * s), 1.6 * s, 1)
    c.rect(x0 - 2 * s, cy - 3 * s, x0, cy + 3 * s, 1)
    c.rect(x1, cy - 3 * s, x1 + 2 * s, cy + 3 * s, 1)
    ey = cy - 3 * s
    ex = (cx - 5 * s, cx + 5 * s)
    if state in ("off", "on", "happy", "sad"):
        for x in ex:
            c.rect(x - 2 * s, ey - 2 * s, x + 2 * s + 1, ey + 2 * s + 1, fg)
    elif state == "suspicious":
        c.rect(ex[0] - 2 * s, ey, ex[0] + 2 * s + 1, ey + 1 * s + 1, fg)       # squint
        c.rect(ex[1] - 2 * s, ey - 2 * s, ex[1] + 2 * s + 1, ey + 2 * s + 1, fg)
        c.line((ex[0] - 3 * s, ey - 3 * s), (ex[0] + 3 * s, ey - 1 * s), 1 * s, fg)  # brow
    elif state == "surprised":
        for x in ex:
            c.ring((x + 0.5, ey + 0.5), 2.6 * s, 1.2 * s, fg)
    my = cy + 4 * s
    if state == "happy":
        c.line((cx - 6 * s, my - 1 * s), (cx - 3 * s, my + 2 * s), 1.2 * s, fg)
        c.line((cx - 3 * s, my + 2 * s), (cx + 3 * s, my + 2 * s), 1.2 * s, fg)
        c.line((cx + 3 * s, my + 2 * s), (cx + 6 * s, my - 1 * s), 1.2 * s, fg)
    elif state == "sad":
        c.line((cx - 6 * s, my + 2 * s), (cx - 3 * s, my - 1 * s), 1.2 * s, fg)
        c.line((cx - 3 * s, my - 1 * s), (cx + 3 * s, my - 1 * s), 1.2 * s, fg)
        c.line((cx + 3 * s, my - 1 * s), (cx + 6 * s, my + 2 * s), 1.2 * s, fg)
    elif state == "surprised":
        c.ring((cx + 0.5, my + 1 * s), 2.5 * s, 1.0 * s, fg)
    else:
        c.rect(cx - 5 * s, my, cx + 6 * s, my + 1 * s + (0 if s == 1 else 1), fg)
    return (x0 - 2 * s, y0 - 6 * s, x1 + 2 * s, y1)


FACE_STATES = ["on", "happy", "suspicious", "surprised", "sad"]
FACE_C = (int(T.CX), 238)


def arrow(c, tip, ang, size=7, filled=False):
    """an insert arrow pointing along `ang` (degrees, y down) with its tip at `tip`"""
    a = math.radians(ang)
    ux, uy = math.cos(a), math.sin(a)
    nx, ny = -uy, ux
    base = (tip[0] - ux * size, tip[1] - uy * size)
    p1 = (base[0] + nx * size * 0.55, base[1] + ny * size * 0.55)
    p2 = (base[0] - nx * size * 0.55, base[1] - ny * size * 0.55)
    m = c.polygon_mask([tip, p1, p2])
    if filled:
        c.fill(m, None, 1)
    else:
        c.fill(m, None, 0)
        c.line(tip, p1, 1)
        c.line(p1, p2, 1)
        c.line(p2, tip, 1)


def label(c, cx, y, s):
    """small label on a white plate"""
    w = len(s) * 6 - 1
    c.rect(int(cx - w / 2) - 2, y - 2, int(cx - w / 2) + w + 2, y + 9, 0)
    c.text_c(cx, y, s)


def draw_table(lit=None, state=None):
    """the playfield. lit: set of lamp names drawn lit; state: dict of exclusive states"""
    lit = lit or set()
    state = state or {}
    T.build()
    c = Canvas(W, H)
    inside = wall_side_fill(c)
    # ---- cabinet side walls (thick, outside the boundary) and the top arcs
    c.arc((T.LEFT_X + T.ARC_R, T.TOP_Y + T.ARC_R), T.ARC_R + 1, 180, 270, 2)
    c.arc((T.RIGHT_X - T.ARC_R, T.TOP_Y + T.ARC_R), T.ARC_R + 1, 270, 360, 2)
    c.line((T.LEFT_X + T.ARC_R, T.TOP_Y - 1), (T.RIGHT_X - T.ARC_R, T.TOP_Y - 1), 2)
    c.line((T.LEFT_X - 1, T.TOP_Y + T.ARC_R), (T.LEFT_X - 1, H), 2)
    c.line((T.RIGHT_X + 1, T.TOP_Y + T.ARC_R), (T.RIGHT_X + 1, H), 2)
    # ---- shooter lane: wall, a light texture in the lane, plunger housing at the bottom
    lane = (c.px > T.SHOOT_X0 + 1.5) & (c.px < T.SHOOT_X1) & (c.py > T.SHOOT_TOP + 8) & (c.py < T.PLUNGER_Y - 10)
    c.fill(lane, P_DOTS)
    c.rect(int(T.SHOOT_X0) + 2, int(T.PLUNGER_Y) + 12, int(T.SHOOT_X1), H, 1)
    c.text(int(T.SHOOT_X0) + 4, 150, "")
    for i, ch in enumerate("LAUNCH"):
        c.text(int(T.SHOOT_X0) + 4, 168 + i * 9, ch)
    # ---- slingshots: shaded triangles with thick rubber
    for side in (0, 1):
        pts = T.SLING_L if side == 0 else [T.mirror(p) for p in T.SLING_L]
        c.fill(c.polygon_mask(pts), P_GRAY25)
        c.line(pts[0], pts[2], 4)
        c.line(pts[0], pts[1], 3)
        c.line(pts[1], pts[2], 3)
    # ---- the apron: everything below the inlane guides is the cabinet's apron, except the
    # drain between the flippers and the outlane chutes
    # ---- bowl: a loss landscape's contour lines around the minimum (the sink)
    bc = T.BOWL_C
    for r in (T.BOWL_R, T.BOWL_R - 5, T.BOWL_R - 10):
        c.circle_dotted(bc, r, every=2 if r > 12 else 1.5)
    c.ring(bc, T.SINK_R + 2, T.SINK_R + 0.5, 1)
    c.disc(bc, T.SINK_R - 0.5, 1)
    c.text_c(bc[0], int(bc[1] + T.BOWL_R + 3), "DESCENT")
    # ---- dropout bank: a roof (house) shape behind the targets
    for s in T.SHAPES:
        if s["kind"] == "SEG" and s["mat"] == "WALL" and s["rad"] == 1.0 and abs(s["a"][1] - s["b"][1]) < 20 \
                and min(s["a"][0], s["b"][0]) >= T.DROP_C[0] - 30 and max(s["a"][0], s["b"][0]) <= T.DROP_C[0] + 30 \
                and min(s["a"][1], s["b"][1]) > T.DROP_C[1] - 20 and max(s["a"][1], s["b"][1]) < T.DROP_C[1]:
            pass
    roof = [(T.DROP_C[0] - 16, T.DROP_C[1] - 2.5), (T.DROP_C[0], T.DROP_C[1] - 11.5), (T.DROP_C[0] + 16, T.DROP_C[1] - 2.5)]
    c.fill(c.polygon_mask(roof), P_GRAY50)
    c.line(roof[0], roof[1], 2)
    c.line(roof[1], roof[2], 2)
    c.line(roof[0], roof[2], 2)
    c.text_c(T.DROP_C[0], int(T.DROP_C[1] + 5), "DROPOUT")
    # ---- ramp (drawn before the walls so posts sit on top): rails and a dotted deck
    x0, x1, x2, x3 = T.RAMP_X
    rc = T.RAMP_TOP_C
    ic = ((x1 + x2) / 2, T.RAMP_TOP_Y)
    ro, ri = (x3 - x0) / 2, (x2 - x1) / 2
    deck = (((c.px > x0) & (c.px < x1) & (c.py > T.RAMP_TOP_Y) & (c.py < T.RAMP_ENTRY_Y + 6)) |
            ((c.px > x2) & (c.px < x3) & (c.py > T.RAMP_TOP_Y) & (c.py < T.RAMP_EXIT_Y)) |
            ((np.hypot(c.px - rc[0], c.py - rc[1]) < ro) & (np.hypot(c.px - ic[0], c.py - ic[1]) > ri) &
             (c.py <= T.RAMP_TOP_Y)))
    c.fill(deck, None, 0)
    c.fill(deck, P_GRAY12)
    c.text_c((x0 + x3) / 2, int(T.RAMP_TOP_Y - ro + 7), "BACKPROP")
    c.a[int(T.RAMP_TOP_Y - ro + 6):int(T.RAMP_TOP_Y - ro + 15), int(x0 + 10):int(x3 - 10)] &= \
        c.a[int(T.RAMP_TOP_Y - ro + 6):int(T.RAMP_TOP_Y - ro + 15), int(x0 + 10):int(x3 - 10)]
    # ---- every wall, rail, guide and post from the physics shapes
    for s in T.SHAPES:
        if s["kind"] == "SEG":
            if s["mat"] == "SLING" or (s["mat"] == "RUBBER" and s["rad"] == 1.5 and s["a"][1] > 200):
                continue                    # sling edges drawn above
            if "DROP" in s["flags"]:
                continue                    # drop targets drawn below
            wdt = 2 * s["rad"] + 1 if s["rad"] else 2
            if s["a"][0] in (T.LEFT_X, T.RIGHT_X) and s["b"][0] == s["a"][0]:
                continue                    # outer walls drawn as the cabinet edge
            if s["mat"] == "RAIL" and "ONEWAY" in s["flags"]:
                c.line(s["a"], s["b"], 1)   # gates: thin wire
                continue
            c.line(s["a"], s["b"], wdt)
        elif s["kind"] == "CIRCLE" and s["mat"] == "RUBBER":
            c.disc(s["c"], s["r"])
        elif s["kind"] == "ARC" and s["layers"] == 2:
            c.arc(s["c"], s["r"], s["a0"], s["a1"], 2)
    # ---- top lanes: letters T R A I N under the lanes (lamps)
    for i, ch in enumerate("TRAIN"):
        cx = (T.LANE_X[i] + T.LANE_X[i + 1]) / 2
        rect = (int(cx) - 5, 58, int(cx) + 6, 69)
        on = ("LANE%d" % i) in lit
        c.rect(*rect, ink=1 if on else 0)
        c.frame(*rect)
        c.text(int(cx) - 2, 60, ch, ink=0 if on else 1)
    # rollover buttons in the lanes
    for i in range(5):
        cx = (T.LANE_X[i] + T.LANE_X[i + 1]) / 2
        c.ring((cx, T.LANE_SENSOR_Y), 2.6, 1.4)
    # ---- pop bumpers ("neurons"): ring, shaded skirt, cap; dendrites around them
    for i, bcx in enumerate(T.BUMPERS):
        r = T.BUMPER_R
        for k in range(8):
            a = math.radians(k * 45 + 22.5)
            c.line((bcx[0] + math.cos(a) * (r + 1), bcx[1] + math.sin(a) * (r + 1)),
                   (bcx[0] + math.cos(a) * (r + 4), bcx[1] + math.sin(a) * (r + 4)), 1)
        c.disc(bcx, r, 0)
        c.ring(bcx, r, r - 2)
        sk = np.hypot(c.px - bcx[0], c.py - bcx[1])
        c.fill((sk <= r - 2) & (sk > 6), P_GRAY50)
        on = ("BUMPER%d" % i) in lit
        c.disc(bcx, 6, 1 if on else 0)
        c.ring(bcx, 6, 5)
        c.disc(bcx, 2, 0 if on else 1)
    # ---- drop targets (up) or their slots (down)
    for s in T.SHAPES:
        if "DROP" in s["flags"]:
            name = "DROPDOWN%d" % (s["id"] - T.ID["DROP0"])
            if name in lit:
                c.line(s["a"], s["b"], 1)
                c.line(s["a"], s["b"], 0.5, 0)
            else:
                c.line(s["a"], s["b"], 4)
                c.line(s["a"], s["b"], 1, 0)
    # ---- orbit lane: spinner housing marks and the ATTENTION label
    c.line((T.LEFT_X + 1, T.SPINNER_Y - 3), (T.LEFT_X + 1, T.SPINNER_Y + 3), 2)
    c.line((T.ORBIT_X - 2, T.SPINNER_Y - 3), (T.ORBIT_X - 2, T.SPINNER_Y + 3), 2)
    c.vtext(int(T.LEFT_X) + 6, int(T.SPINNER_Y) + 10, "")
    # labels
    c.text(26, int(T.ORBIT_BOT) - 30, "")
    # ---- lamps ------------------------------------------------------------------------
    # epoch multipliers between the slings
    for i, m in enumerate(("2X", "3X", "4X", "5X")):
        x = int(T.CX) - 34 + i * 18
        on = ("EPOCH%d" % (i + 2)) in lit
        c.rect(x - 1, 206, x + 14, 216, 1 if on else 0)
        c.frame(x - 1, 206, x + 14, 216)
        c.text(x + 2, 208, m, ink=0 if on else 1)
    # shot arrows
    arrows = {
        "ARROW_RAMP": ((T.RAMP_X[0] + T.RAMP_X[1]) / 2, T.RAMP_ENTRY_Y + 16, 270),

        "ARROW_DROP": (T.DROP_C[0], T.DROP_C[1] + 22, 270),
        "ARROW_SINK": (T.BOWL_C[0], T.BOWL_C[1] + T.BOWL_R + 22, 270),
    }
    for name, (x, y, a) in arrows.items():
        arrow(c, (x, y), a, 7, name in lit)
    # the orbit's lamp: a round insert by the funnel (an outlined arrow there looked like the
    # mouse pointer)
    on = "ARROW_ORBIT" in lit
    c.disc((38.0, 150.0), 4.6, 1)
    c.disc((38.0, 150.0), 3.2, 1 if on else 0)
    c.disc((38.0, 150.0), 1.2, 0 if on else 1)
    # LOCK (bowl) and JACKPOT (ramp) inserts
    for name, cx, y, txt in (("LOCK", T.BOWL_C[0] - 34, T.BOWL_C[1] - 4, "LOCK"),
                             ("JACKPOT", (T.RAMP_X[0] + T.RAMP_X[1]) / 2, T.RAMP_ENTRY_Y + 24, "JACK")):
        w = len(txt) * 6 + 3
        on = name in lit
        x0 = int(cx - w / 2)
        c.rect(x0, int(y) - 2, x0 + w, int(y) + 9, 1 if on else 0)
        c.frame(x0, int(y) - 2, x0 + w, int(y) + 9)
        c.text(x0 + 2, int(y), txt, ink=0 if on else 1)
    # kickback (left outlane) and shoot again (bottom)
    on = "KICKBACK" in lit
    arrow(c, (12.0, 288.0), 270, 6, on)
    c.text(7, 296, "K", ink=1)
    on = "AGAIN" in lit
    c.rect(int(T.CX) - 16, 311, int(T.CX) + 17, 321, 1 if on else 0)
    c.frame(int(T.CX) - 16, 311, int(T.CX) + 17, 321)
    c.text(int(T.CX) - 14, 313, "AGAIN", ink=0 if on else 1)
    # robot face (Turing test)
    draw_robot_face(c, FACE_C[0], FACE_C[1], state.get("FACE", "off"))
    c.text_c(T.CX, 252, "TURING")
    return c


LAMP_NAMES = (["LANE%d" % i for i in range(5)] + ["BUMPER%d" % i for i in range(3)] +
              ["DROPDOWN%d" % i for i in range(3)] + ["EPOCH%d" % i for i in range(2, 6)] +
              ["ARROW_RAMP", "ARROW_ORBIT", "ARROW_DROP", "ARROW_SINK", "LOCK", "JACKPOT", "KICKBACK", "AGAIN"])


def xor_patch(base, other):
    """byte-aligned rectangle covering the differing pixels, and its XOR bits"""
    d = base ^ other
    ys, xs = np.nonzero(d)
    if len(ys) == 0:
        return None
    y0, y1 = int(ys.min()), int(ys.max()) + 1
    x8a, x8b = int(xs.min()) // 8, int(xs.max()) // 8 + 1
    return (x8a, x8b - x8a, y0, y1 - y0, d[y0:y1, x8a * 8:x8b * 8])


def flipper_frames(nframes=16):
    """pre-rendered flipper sprites: mask (body grown by 1 px) and image, byte-aligned,
    one per angle step from rest to up"""
    out = []
    for f in T.flippers():
        px, py = f["pivot"]
        frames = []
        for k in range(nframes):
            ang = f["rest"] + (f["up"] - f["rest"]) * k / (nframes - 1)
            a = math.radians(ang)
            tip = (px + T.FLIP_LEN * math.cos(a), py + T.FLIP_LEN * math.sin(a))
            c = Canvas(W, H)
            # tapered body: union of discs along the axis
            d = c.seg_dist((px, py), tip)
            # radius varies along the axis
            ux, uy = math.cos(a), math.sin(a)
            t = np.clip((c.px - px) * ux + (c.py - py) * uy, 0, T.FLIP_LEN) / T.FLIP_LEN
            r = T.FLIP_RB + (T.FLIP_RT - T.FLIP_RB) * t
            body = d <= r + 0.15
            mask = d <= r + 1.2
            img = body.copy()
            # a white groove along the flipper and a white pivot dot
            img &= ~((c.seg_dist((px + ux * 5, py + uy * 5), (px + ux * (T.FLIP_LEN - 5), py + uy * (T.FLIP_LEN - 5))) <= 0.5))
            img &= ~(np.hypot(c.px - px, c.py - py) <= 1.3)
            frames.append((mask.astype(np.uint8), img.astype(np.uint8)))
        out.append(frames)
    return out


def sprite_rect(frames):
    """common byte-aligned rectangle of a list of (mask, img) full-size frames"""
    m = np.zeros_like(frames[0][0])
    for mask, _ in frames:
        m |= mask
    ys, xs = np.nonzero(m)
    y0, y1 = int(ys.min()), int(ys.max()) + 1
    x8a, x8b = int(xs.min()) // 8, int(xs.max()) // 8 + 1
    return x8a, x8b - x8a, y0, y1 - y0


def plunger_frames():
    frames = []
    x0, x1 = int(T.SHOOT_X0) + 3, int(T.SHOOT_X1) - 1
    for pull in range(int(T.PLUNGER_PULL) + 1):
        c = Canvas(W, H)
        tip = int(T.PLUNGER_Y) + pull
        img = np.zeros((H, W), dtype=np.uint8)
        img[tip:tip + 3, x0:x1] = 1                       # tip cap
        img[tip + 3:H, x0 + 3:x1 - 3] = 1                 # rod
        for y in range(tip + 6, H, 3):                    # spring coils
            img[y, x0 + 1:x1 - 1] = 1
        mask = np.zeros((H, W), dtype=np.uint8)
        mask[int(T.PLUNGER_Y) - 1:H, int(T.SHOOT_X0) + 2:int(T.SHOOT_X1)] = 1   # erase the lane bottom
        frames.append((mask, img))
        del c
    return frames


def spinner_frames():
    frames = []
    y = T.SPINNER_Y
    x0, x1 = T.LEFT_X + 1, T.ORBIT_X - 2
    for k in range(4):
        c = Canvas(W, H)
        half = [0.5, 1.5, 2.5, 1.5][k]
        img = np.zeros((H, W), dtype=np.uint8)
        m = (c.px >= x0) & (c.px <= x1) & (np.abs(c.py - y) <= half + 0.01)
        img[m] = 1
        if k == 2:
            img[(c.px >= x0 + 2) & (c.px <= x1 - 2) & (np.abs(c.py - y) <= 0.6)] = 0
        mask = ((c.px >= x0) & (c.px <= x1) & (np.abs(c.py - y) <= 3.6)).astype(np.uint8)
        frames.append((mask, img))
    return frames


BALL_D = 10


def ball_sprite():
    c = Canvas(BALL_D, BALL_D)
    d = np.hypot(c.px - BALL_D / 2, c.py - BALL_D / 2)
    mask = (d <= 5.0).astype(np.uint8)
    img = (d <= 4.05).astype(np.uint8)
    img[3, 3] = 0
    img[3, 4] = 0
    img[4, 3] = 0
    return mask, img


# ------------------------------------------------------------------------------ the panel
def draw_panel():
    c = Canvas(PANEL_W, H)
    c.fill(np.ones((H, PANEL_W), dtype=bool), P_CABINET)
    c.rect(0, 0, 3, H, 1)
    # logo plate
    c.rect(6, 4, PANEL_W - 6, 52, 0)
    c.frame(6, 4, PANEL_W - 6, 52)
    c.frame(8, 6, PANEL_W - 8, 50)
    c.text(14, 11, "NEURAL", scale=2)
    c.text(14, 30, "PINBALL", scale=2)
    # a small neural network beside the name
    layers = [[16, 28, 40], [12, 24, 36, 44], [22, 34]]
    xs = [124, 148, 172]
    pts = [[(xs[i], y + 4) for y in col] for i, col in enumerate(layers)]
    for a_col, b_col in zip(pts, pts[1:]):
        for a in a_col:
            for b in b_col:
                c.line(a, b, 1)
    for col in pts:
        for p in col:
            c.disc(p, 3.2, 0)
            c.ring(p, 3.2, 2.0)
    # DMD window (drawn by the game)
    c.rect(4, DMD_Y - 4, PANEL_W - 4, DMD_Y + DMD_H + 4, 1)
    c.frame(2, DMD_Y - 6, PANEL_W - 2, DMD_Y + DMD_H + 6, 0)
    # info plate (drawn by the game)
    c.rect(6, INFO_Y, PANEL_W - 6, H - 6, 0)
    c.frame(6, INFO_Y, PANEL_W - 6, H - 6)
    return c


DMD_Y, DMD_H = 62, 66          # 22 dot rows at 3 px
INFO_Y = DMD_Y + DMD_H + 12
JUDGE_C = (40, INFO_Y + 34)


def judge_faces():
    """the big robot judge on the panel, one image per state (byte-aligned common rect)"""
    out = []
    for st in ["off"] + FACE_STATES:
        c = Canvas(PANEL_W, H)
        c.rect(JUDGE_C[0] - 34, JUDGE_C[1] - 30, JUDGE_C[0] + 34, JUDGE_C[1] + 26, 0)
        draw_robot_face(c, JUDGE_C[0], JUDGE_C[1], st if st != "off" else "off", scale=2)
        out.append(c.a.copy())
    return out


# ------------------------------------------------------------------------------ C output
def bits_c(arr):
    """rows of bits -> bytes, MSB = leftmost pixel"""
    h, w = arr.shape
    assert w % 8 == 0
    return np.packbits(arr.astype(np.uint8), axis=1).flatten()


def c_bytes(name, data, per_line=24):
    lines = ["const unsigned char %s[%d] = {" % (name, len(data))]
    for i in range(0, len(data), per_line):
        lines.append("    " + ",".join(str(int(v)) for v in data[i:i + per_line]) + ",")
    lines.append("};")
    return lines


def emit():
    """C declarations (header lines, source lines) for gen.py"""
    base = draw_table()
    h = ["", "/* ---- art (tools/art.py) ---- */",
         "#define PANEL_W %d" % PANEL_W, "#define DMD_Y %d" % DMD_Y, "#define DMD_H %d" % DMD_H,
         "#define INFO_Y %d" % INFO_Y, "#define BALL_D %d" % BALL_D,
         "typedef struct { unsigned char x8, w8; short y, h; const unsigned char *bits; } Patch;",
         "typedef struct { unsigned char x8, w8; short y, h; const unsigned char *mask, *img; } Sprite;",
         "extern const unsigned char bg_bits[%d];" % (H * W // 8),
         "extern const unsigned char panel_bits[%d];" % (H * PANEL_W // 8)]
    c = [""]
    c += c_bytes("bg_bits", bits_c(base.a))
    c += c_bytes("panel_bits", bits_c(draw_panel().a))
    # lamps (XOR patches)
    h.append("enum {")
    names = []
    for name in LAMP_NAMES:
        names.append(name)
        h.append("    LAMP_%s," % name)
    h.append("    NLAMPS };")
    h.append("enum { FACE_OFF = 0, %s, NFACES };" % ", ".join("FACE_" + s.upper() for s in FACE_STATES))
    h.append("extern const Patch lamp_patch[NLAMPS];")
    h.append("extern const Patch face_patch[NFACES];          /* XOR against FACE_OFF (the background) */")
    plist = []
    for name in names:
        other = draw_table(lit={name})
        p = xor_patch(base.a, other.a)
        assert p, name
        c += c_bytes("lamp_%s" % name.lower(), bits_c(p[4]))
        plist.append("    {%d, %d, %d, %d, lamp_%s}," % (p[0], p[1], p[2], p[3], name.lower()))
    c.append("const Patch lamp_patch[NLAMPS] = {")
    c += plist
    c.append("};")
    flist = ["    {0, 0, 0, 0, 0},"]
    for st in FACE_STATES:
        other = draw_table(state={"FACE": st})
        p = xor_patch(base.a, other.a)
        c += c_bytes("face_%s" % st, bits_c(p[4]))
        flist.append("    {%d, %d, %d, %d, face_%s}," % (p[0], p[1], p[2], p[3], st))
    c.append("const Patch face_patch[NFACES] = {")
    c += flist
    c.append("};")
    # the big judge faces on the panel: full images of a common rect
    jf = judge_faces()
    ys, xs = np.nonzero(np.any([f ^ jf[0] for f in jf[1:]], axis=0) | (jf[0] != 0))
    jy0 = JUDGE_C[1] - 30
    jy1 = JUDGE_C[1] + 26
    jx8a, jx8b = (JUDGE_C[0] - 34) // 8, (JUDGE_C[0] + 34) // 8 + 1
    for i, f in enumerate(jf):
        c += c_bytes("judge_%d" % i, bits_c(f[jy0:jy1, jx8a * 8:jx8b * 8]))
    h.append("extern const Patch judge_img[NFACES];          /* full images (not XOR) on the panel */")
    c.append("const Patch judge_img[NFACES] = {")
    for i in range(len(jf)):
        c.append("    {%d, %d, %d, %d, judge_%d}," % (jx8a, jx8b - jx8a, jy0, jy1 - jy0, i))
    c.append("};")
    # sprites
    def sprites(name, frames, tight=False):
        x8, w8, y0, hh = sprite_rect(frames)
        out = []
        for k, (mask, img) in enumerate(frames):
            if tight:                                   # this frame's own rectangle
                x8, w8, y0, hh = sprite_rect([(mask, img)])
            mm = mask[y0:y0 + hh, x8 * 8:(x8 + w8) * 8]
            ii = img[y0:y0 + hh, x8 * 8:(x8 + w8) * 8] & mm
            c.extend(c_bytes("%s_m%d" % (name, k), bits_c(mm)))
            c.extend(c_bytes("%s_i%d" % (name, k), bits_c(ii)))
            out.append("    {%d, %d, %d, %d, %s_m%d, %s_i%d}," % (x8, w8, y0, hh, name, k, name, k))
        return out
    ff = flipper_frames()
    h.append("#define NFLIPFRAMES %d" % len(ff[0]))
    h.append("extern const Sprite flip_sprite[2][NFLIPFRAMES];")
    c.append("")
    lines = []
    for i, frames in enumerate(ff):
        lines.append(sprites("flip%d" % i, frames, tight=True))
    c.append("const Sprite flip_sprite[2][NFLIPFRAMES] = {")
    for l in lines:
        c.append("  {")
        c += l
        c.append("  },")
    c.append("};")
    pf = plunger_frames()
    h.append("#define NPLUNGER %d" % len(pf))
    h.append("extern const Sprite plunger_sprite[NPLUNGER];")
    l = sprites("plg", pf)
    c.append("const Sprite plunger_sprite[NPLUNGER] = {")
    c += l
    c.append("};")
    sf = spinner_frames()
    h.append("extern const Sprite spinner_sprite[4];")
    l = sprites("spn", sf)
    c.append("const Sprite spinner_sprite[4] = {")
    c += l
    c.append("};")
    bm, bi = ball_sprite()
    h.append("extern const unsigned short ball_mask[BALL_D], ball_img[BALL_D];   /* left-aligned rows */")
    c.append("const unsigned short ball_mask[BALL_D] = {%s};" % ",".join(
        str(int("".join(str(v) for v in row) + "0" * (16 - BALL_D), 2)) for row in bm))
    c.append("const unsigned short ball_img[BALL_D] = {%s};" % ",".join(
        str(int("".join(str(v) for v in row) + "0" * (16 - BALL_D), 2)) for row in bi))
    # font: 7 rows of 5 bits per glyph, in font57.ORDER
    h.append('#define FONT_ORDER "%s"' % font57.ORDER.replace("\\", "\\\\").replace('"', '\\"'))
    h.append("extern const unsigned char font57[%d][7];" % len(font57.ORDER))
    c.append("const unsigned char font57[%d][7] = {" % len(font57.ORDER))
    for ch in font57.ORDER:
        g = font57.GLYPHS[ch]
        c.append("    {%s}," % ",".join(str(int(r.replace("#", "1").replace(".", "0"), 2)) for r in g))
    c.append("};")
    return h, c


def save_png(arr, path, scale=2):
    from PIL import Image
    im = Image.fromarray(((1 - arr) * 255).astype(np.uint8))
    im = im.resize((arr.shape[1] * scale, arr.shape[0] * scale), Image.NEAREST)
    im.save(path)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "build")
    os.makedirs(out, exist_ok=True)
    base = draw_table()
    save_png(base.a, os.path.join(out, "table_art.png"))
    lit = draw_table(lit=set(LAMP_NAMES) - {"DROPDOWN0", "DROPDOWN1", "DROPDOWN2"}, state={"FACE": "happy"})
    # with sprites: flippers, plunger, a ball
    a = lit.a.copy()
    for frames in flipper_frames():
        m, i = frames[0]
        a = (a & (1 - m)) | i
    m, i = plunger_frames()[0]
    a = (a & (1 - m)) | i
    m, i = spinner_frames()[2]
    a = (a & (1 - m)) | i
    bm, bi = ball_sprite()
    for (bx, by) in ((150, 230), (245, 293)):
        sub = a[by:by + BALL_D, bx:bx + BALL_D]
        a[by:by + BALL_D, bx:bx + BALL_D] = (sub & (1 - bm)) | bi
    save_png(a, os.path.join(out, "table_lit.png"))
    panel = draw_panel()
    comp = np.concatenate([a, panel.a], axis=1)
    save_png(comp, os.path.join(out, "screen_mock.png"))
    print("wrote", out)


if __name__ == "__main__":
    main()

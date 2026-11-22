#!/usr/bin/env python3
"""Synthetic handwriting for Scribble Calc's 18 symbols, and the "mouse" degradation that every
training sample (real or synthetic) goes through before it is rasterised.

Glyphs are drawn in units where a digit is 1 tall (y down, 0 = top, 1 = baseline). Every glyph
has several styles; control points are jittered and joined with Catmull-Rom splines. All code and
shapes are original (MIT, like the rest of the repo).
"""
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from strokes import CLASSES, raster_fast


def catmull(pts, n_per=8):
    """Catmull-Rom spline through control points pts (k, 2) -> dense polyline."""
    p = np.asarray(pts, np.float64)
    if len(p) < 3:
        t = np.linspace(0, 1, max(2, n_per))[:, None]
        return p[0] + (p[-1] - p[0]) * t
    q = np.vstack([2 * p[0] - p[1], p, 2 * p[-1] - p[-2]])
    out = []
    for i in range(1, len(q) - 2):
        p0, p1, p2, p3 = q[i - 1], q[i], q[i + 1], q[i + 2]
        for t in np.linspace(0, 1, n_per, endpoint=False):
            t2, t3 = t * t, t * t * t
            out.append(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2
                              + (-p0 + 3 * p1 - 3 * p2 + p3) * t3))
    out.append(p[-1])
    return np.array(out)


def ellipse(cx, cy, rx, ry, a0, a1, n=24):
    a = np.radians(np.linspace(a0, a1, n))
    return np.stack([cx + rx * np.cos(a), cy + ry * np.sin(a)], 1)


def line(p0, p1, n=2):
    t = np.linspace(0, 1, n)[:, None]
    return np.asarray(p0, np.float64) + (np.asarray(p1, np.float64) - np.asarray(p0, np.float64)) * t


class Glyphs:
    def __init__(self, rng):
        self.r = rng

    def j(self, pts, s=0.045):
        p = np.asarray(pts, np.float64)
        return p + self.r.normal(0, s, p.shape)

    def u(self, a, b):
        return self.r.uniform(a, b)

    def c(self, *opts):
        return opts[self.r.integers(len(opts))]

    def sp(self, pts, s=0.045):
        return catmull(self.j(pts, s))

    # ---- digits
    def d0(self):
        rx, ry = self.u(0.18, 0.34), self.u(0.46, 0.52)
        a0 = self.u(-120, -60)
        sweep = self.u(330, 400) * self.c(1, -1)
        e = ellipse(0, 0.5, rx, ry, a0, a0 + sweep, 28)
        e += self.r.normal(0, 0.02, e.shape)
        # slight egg shape / tilt
        e[:, 0] += (e[:, 1] - 0.5) * self.u(-0.15, 0.15)
        return [e]

    def d1(self):
        top, bot = (self.u(-0.03, 0.03), 0.0), (self.u(-0.06, 0.03), 1.0)
        st = self.c(0, 0, 1, 1, 2)
        if st == 0:
            return [line(self.j(top, 0.02), self.j(bot, 0.02), 6)]
        flag = (top[0] - self.u(0.12, 0.25), self.u(0.15, 0.3))
        s = [self.sp([flag, top, (top[0] * 0.5 + bot[0] * 0.5, 0.5), bot], 0.015)]
        if st == 2:
            w = self.u(0.12, 0.22)
            s.append(line(self.j((bot[0] - w, 1.0), 0.02), self.j((bot[0] + w, 1.0), 0.02), 4))
        return s

    def d2(self):
        w = self.u(0.2, 0.3)
        if self.r.random() < 0.75:
            return [self.sp([(-w, 0.25), (-w * 0.3, 0.02), (w * 0.7, 0.05), (w, 0.3), (w * 0.3, 0.6),
                             (-w, 1.0), (0, 0.98), (w * 1.1, 0.99)])]
        # with a small loop at the bottom left
        return [self.sp([(-w, 0.22), (0, 0.0), (w, 0.25), (0, 0.65), (-w, 0.98), (-w * 0.6, 0.85),
                         (-w * 0.4, 1.0), (w * 1.1, 1.0)])]

    def d3(self):
        w = self.u(0.2, 0.3)
        if self.r.random() < 0.75:
            return [self.sp([(-w, 0.12), (0, 0.0), (w, 0.2), (-w * 0.2, 0.47), (w * 0.2, 0.5),
                             (w * 1.05, 0.75), (0, 1.0), (-w, 0.88)])]
        return [self.sp([(-w, 0.0), (w, 0.0), (-w * 0.1, 0.42), (w, 0.62), (w * 0.4, 0.97), (-w, 0.88)], 0.035)]

    def d4(self):
        w = self.u(0.2, 0.3)
        cy = self.u(0.55, 0.7)
        st = self.c(0, 0, 1, 2)
        xv = self.u(0.0, 0.2)
        if st == 0:      # open: down-and-across, then the vertical
            return [self.sp([(-w * 0.6, 0.0), (-w, cy), (w, cy)], 0.03),
                    line(self.j((xv, self.u(0.1, 0.3)), 0.03), self.j((xv, 1.0), 0.03), 6)]
        if st == 1:      # closed, one stroke: up the vertical, diagonal down, across
            return [self.sp([(xv, 1.0), (xv, 0.5), (xv, 0.0), (-w, cy), (w * 1.1, cy)], 0.025)]
        return [self.sp([(-w, 0.0), (-w * 1.05, cy), (w, cy)], 0.03),      # open top
                line(self.j((xv, 0.0), 0.03), self.j((xv, 1.0), 0.03), 6)]

    def d5(self):
        w = self.u(0.2, 0.3)
        body = [(-w * 0.7, 0.0), (-w * 0.8, 0.45), (0, 0.38), (w, 0.62), (w * 0.4, 0.95), (-w, 0.9)]
        if self.r.random() < 0.6:
            return [self.sp(body, 0.035), line(self.j((-w * 0.7, 0.0), 0.03), self.j((w, 0.0), 0.03), 4)]
        return [self.sp([(w, 0.0)] + body, 0.035)]

    def d6(self):
        w = self.u(0.2, 0.3)
        return [self.sp([(w * 0.8, 0.0), (-w * 0.6, 0.35), (-w, 0.75), (0, 1.0), (w, 0.78), (0.05, 0.52),
                         (-w * 0.9, 0.7)], 0.04)]

    def d7(self):
        w = self.u(0.2, 0.32)
        st = self.c(0, 0, 1, 2)
        bot = self.u(-0.15, 0.05)
        top = [(-w, 0.0), (w, 0.0)]
        if st == 2:
            top = [(-w * 1.05, 0.15)] + top
        s = [self.sp(top + [(bot * 0.5 + w * 0.5, 0.5), (bot, 1.0)], 0.025)]
        if st == 1:
            s.append(line(self.j((-w * 0.4, 0.52), 0.03), self.j((w * 0.6, 0.48), 0.03), 4))
        return s

    def d8(self):
        w = self.u(0.18, 0.27)
        if self.r.random() < 0.75:
            return [self.sp([(w * 0.8, 0.12), (0, 0.0), (-w, 0.2), (0, 0.46), (w, 0.72), (0, 1.0), (-w, 0.75),
                             (0, 0.46), (w * 0.9, 0.2), (w * 0.2, 0.0)], 0.035)]
        rt = self.u(0.2, 0.24)
        a = self.r.uniform(-120, -60)
        return [ellipse(0, rt, w * 0.85, rt, a, a + 360, 18) + self.r.normal(0, 0.015, (18, 2)),
                ellipse(0, 2 * rt + (1 - 2 * rt) / 2, w, (1 - 2 * rt) / 2, a, a - 360, 18)
                + self.r.normal(0, 0.015, (18, 2))]

    def d9(self):
        w = self.u(0.2, 0.3)
        loop = [(w, 0.12), (0, 0.0), (-w, 0.18), (-w * 0.2, 0.42), (w, 0.28)]
        if self.r.random() < 0.6:
            return [self.sp(loop + [(w * 1.05, 0.05), (w * 0.95, 0.5), (w * 0.8, 1.0)], 0.03)]
        return [self.sp(loop + [(w * 1.05, 0.45), (w * 0.6, 0.9), (0, 1.0), (-w * 0.6, 0.9)], 0.035)]

    # ---- operators (sizes relative to a digit; vertical middle around 0.5)
    def plus(self):
        s = self.u(0.35, 0.9)
        cx, cy = self.u(-0.05, 0.05) * s, 0.5 + self.u(-0.05, 0.05) * s
        v = line(self.j((cx, 0.5 - s / 2), 0.03 * s), self.j((cx, 0.5 + s / 2), 0.03 * s), 5)
        h = line(self.j((-s / 2 * self.u(0.8, 1.2), cy), 0.03 * s), self.j((s / 2 * self.u(0.8, 1.2), cy), 0.03 * s), 5)
        return [v, h] if self.r.random() < 0.5 else [h, v]

    def minus(self):
        L = self.u(0.25, 0.85)
        return [line(self.j((-L / 2, 0.5), 0.02), self.j((L / 2, 0.5), 0.02), 5)]

    def times(self):
        s = self.u(0.3, 0.8)
        a = line(self.j((-s / 2, 0.5 - s / 2), 0.04 * s), self.j((s / 2, 0.5 + s / 2), 0.04 * s), 5)
        b = line(self.j((s / 2, 0.5 - s / 2), 0.04 * s), self.j((-s / 2, 0.5 + s / 2), 0.04 * s), 5)
        if self.r.random() < 0.15:          # cursive-ish: two arcs back to back ")("
            a = self.sp([(-s / 2, 0.5 - s / 2), (-s * 0.05, 0.5), (-s / 2, 0.5 + s / 2)], 0.02)
            b = self.sp([(s / 2, 0.5 - s / 2), (s * 0.05, 0.5), (s / 2, 0.5 + s / 2)], 0.02)
        return [a, b] if self.r.random() < 0.7 else [b, a]

    def dot(self, cx, cy, size):
        k = self.c(0, 0, 1, 2)
        if k == 0:
            return np.array([[cx, cy]])
        if k == 1:
            return line((cx, cy), (cx + self.u(-1, 1) * size, cy + self.u(-1, 1) * size), 2)
        a = self.u(0, 360)
        return ellipse(cx, cy, size * 0.6, size * 0.6, a, a + self.u(200, 400), 6)

    def div(self):
        L = self.u(0.3, 0.8)
        g = self.u(0.12, 0.3)
        sz = self.u(0.01, 0.06)
        bar = line(self.j((-L / 2, 0.5), 0.02), self.j((L / 2, 0.5), 0.02), 5)
        d1 = self.dot(self.u(-0.06, 0.06), 0.5 - g, sz)
        d2 = self.dot(self.u(-0.06, 0.06), 0.5 + g, sz)
        order = self.c(0, 1, 2)
        return [[bar, d1, d2], [d1, bar, d2], [d1, d2, bar]][order]

    def eq(self):
        L1, L2 = self.u(0.3, 0.8), None
        L2 = L1 * self.u(0.75, 1.25)
        g = self.u(0.1, 0.32)
        o = self.u(-0.08, 0.08)
        return [line(self.j((-L1 / 2, 0.5 - g / 2), 0.02), self.j((L1 / 2, 0.5 - g / 2), 0.02), 5),
                line(self.j((-L2 / 2 + o, 0.5 + g / 2), 0.02), self.j((L2 / 2 + o, 0.5 + g / 2), 0.02), 5)]

    def paren(self, side):
        hgt = self.u(0.85, 1.25)
        b = self.u(0.08, 0.22) * side
        top, bot = 0.5 - hgt / 2, 0.5 + hgt / 2
        return [self.sp([(0, top), (-b, 0.5), (0, bot)], 0.02)]

    def point(self):
        return [self.dot(0, 1.0, self.u(0.01, 0.1))]

    def make(self, label):
        f = {"+": self.plus, "-": self.minus, "x": self.times, "/": self.div, "=": self.eq,
             "(": lambda: self.paren(1), ")": lambda: self.paren(-1), ".": self.point}.get(label)
        if f is None:
            f = getattr(self, "d" + label)
        return [np.asarray(s, np.float64) for s in f()]


def mouse_path(p, hpx, rng, clean=False):
    """One stroke already in screen pixels (float): mouse-rate resampling, wobble, hooks.
    Returns int points with consecutive duplicates removed."""
    if len(p) > 1:
        seg = np.sqrt((np.diff(p, axis=0) ** 2).sum(1))
        L = seg.sum()
        if L > 1.5:
            cum = np.concatenate([[0], np.cumsum(seg)])
            step = rng.uniform(1.0, 7.0) if not clean else rng.uniform(1.0, 3.0)
            n = max(2, int(L / step) + 1)
            tt = np.cumsum(rng.uniform(0.6, 1.4, n))
            tt = (tt - tt[0]) / (tt[-1] - tt[0]) * L
            x = np.interp(tt, cum, p[:, 0])
            y = np.interp(tt, cum, p[:, 1])
            if not clean:
                for dim in (x, y):          # slow wobble (hand) + a little fast tremor
                    A = rng.uniform(0, 0.025) * hpx
                    wl = rng.uniform(0.4, 1.5) * hpx
                    dim += A * np.sin(2 * math.pi * tt / wl + rng.uniform(0, 6.3))
                    A = rng.uniform(0, 0.008) * hpx
                    wl = rng.uniform(0.08, 0.25) * hpx
                    dim += A * np.sin(2 * math.pi * tt / wl + rng.uniform(0, 6.3))
            p = np.stack([x, y], 1)
            if not clean and rng.random() < 0.15:     # a hook at one end
                end = rng.random() < 0.5
                a = rng.uniform(0, 2 * math.pi)
                hl = rng.uniform(2, 6)
                hk = np.array([[math.cos(a) * hl, math.sin(a) * hl]])
                p = np.vstack([p, p[-1] + hk]) if end else np.vstack([p[0] + hk, p])
        else:
            p = p[:1]
    q = np.round(p).astype(np.int64)
    keep = np.ones(len(q), bool)
    keep[1:] = (np.diff(q, axis=0) != 0).any(1)
    return q[keep]


def degrade(strokes, href, rng, clean=False):
    """Random slant/rotation/aspect, per-stroke placement error, scale to a random on-screen size,
    mouse-like resampling (positions sampled at the mouse rate along the path), smooth wobble,
    end hooks, then integer pixels. Returns (int strokes, minD)."""
    th = np.radians(np.clip(rng.normal(0, 6), -15, 15))
    k = rng.uniform(-0.3, 0.2)
    ax = rng.uniform(0.8, 1.2)
    c, s = math.cos(th), math.sin(th)
    M = np.array([[c, -s], [s, c]]) @ np.array([[ax, k], [0, 1]])
    pts = np.concatenate(strokes)
    ctr = (pts.min(0) + pts.max(0)) / 2
    hpx = math.exp(rng.uniform(math.log(22), math.log(90)))
    f = hpx / href
    out = []
    multi = len(strokes) > 1
    for st in strokes:
        p = (st - ctr) @ M.T
        if multi and not clean:
            p = p + rng.normal(0, 0.02 * href, 2)
        p = p * f
        out.append(mouse_path(p, hpx, rng, clean))
    minD = max(int(hpx) // 2, 1)
    return out, minD


def sample_raster(strokes, href, rng, clean=False):
    s, minD = degrade(strokes, href, rng, clean)
    return raster_fast(s, minD)


if __name__ == "__main__":
    from strokes import ascii_art
    rng = np.random.default_rng(1)
    g = Glyphs(rng)
    for lab in CLASSES:
        imgs = [sample_raster(g.make(lab), 1.0, rng) for _ in range(4)]
        print(lab)
        rows = [ascii_art(im).split("\n") for im in imgs]
        for r in zip(*rows):
            print("  ".join(r))

#!/usr/bin/env python3
"""How Guess My Doodle turns a drawing into the classifier's input (the spec).

The Mac (app/raster.c), this reference and the GPU training code (train/qd.py) all follow it
exactly, in integer arithmetic.

A drawing is a list of points (x, y) in canvas pixels (any non-negative ints below 4096) and,
for every point, whether the pen is lifted after it (``up[k]``). Segment k joins point k and
point k+1 unless ``up[k]``.

1. Normalise: bounding box of all points; side = max(w, h, MINSIDE); the drawing is scaled by
   E / side and centred in a G x G grid with a MARGIN-pixel border:
       ex = round(w E / side), ox = MARGIN + (E - ex) // 2       (same for y)
       gx = ox + round((x - minx) E / side)                       round(a / b) = (2a + b) // 2b
   so every point lands in [MARGIN, MARGIN + E].
2. Rasterise: every point's pixel is set, and every segment is drawn with
       n = max(|dx|, |dy|);  p_i = a + sign(d) * ((2 i |d| + n) // 2n),  i = 0..n
3. The input is the G x G binary image (row-major, y down).

Partial drawings (a prefix of the points, possibly ending mid-stroke) are normalised by their
own bounding box: the Mac sees exactly what has been drawn so far.
"""
G = 32
MARGIN = 1
E = G - 1 - 2 * MARGIN          # 29
MINSIDE = 48                    # canvas pixels: a tiny first stroke is not blown up to full size


def rdiv(a, b):
    """round(a / b) for a >= 0, b > 0 (halves round up)."""
    return (2 * a + b) // (2 * b)


def grid_points(xs, ys):
    minx, maxx, miny, maxy = min(xs), max(xs), min(ys), max(ys)
    w, h = maxx - minx, maxy - miny
    side = max(w, h, MINSIDE)
    ox = MARGIN + (E - rdiv(w * E, side)) // 2
    oy = MARGIN + (E - rdiv(h * E, side)) // 2
    return ([ox + rdiv((x - minx) * E, side) for x in xs],
            [oy + rdiv((y - miny) * E, side) for y in ys])


def line(img, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    n = max(abs(dx), abs(dy))
    if n == 0:
        img[ay][ax] = 1
        return
    sx, sy = (1 if dx >= 0 else -1), (1 if dy >= 0 else -1)
    adx, ady = abs(dx), abs(dy)
    for i in range(n + 1):
        img[ay + sy * ((2 * i * ady + n) // (2 * n))][ax + sx * ((2 * i * adx + n) // (2 * n))] = 1


def rasterize(xs, ys, up):
    """xs, ys: point coordinates; up[k]: pen lifted after point k. Returns G rows of G ints."""
    img = [[0] * G for _ in range(G)]
    if not xs:
        return img
    gx, gy = grid_points(xs, ys)
    for k in range(len(gx)):
        img[gy[k]][gx[k]] = 1
        if k + 1 < len(gx) and not up[k]:
            line(img, gx[k], gy[k], gx[k + 1], gy[k + 1])
    return img


def from_rows(rows, t=None):
    """Quick, Draw! prep rows (dx, dy, lift) -> points (first point implicit at 0, 0), the
    first t points (default all). Coordinates are shifted to be non-negative."""
    xs, ys, up = [0], [0], [0]
    for dx, dy, lift in rows:
        xs.append(xs[-1] + int(dx))
        ys.append(ys[-1] + int(dy))
        up.append(int(lift))
    if t is not None:
        xs, ys, up = xs[:t], ys[:t], up[:t]
    mx, my = min(xs), min(ys)
    return [x - mx for x in xs], [y - my for y in ys], up


def pack(img):
    """G x G bits -> bytes, row-major, MSB first (the Mac's bitmap layout: 4 bytes per row)."""
    out = bytearray()
    for row in img:
        for b in range(0, G, 8):
            v = 0
            for x in range(b, b + 8):
                v = v << 1 | row[x]
            out.append(v)
    return bytes(out)


def show(img):
    return "\n".join("".join("#" if v else "." for v in row) for row in img)


if __name__ == "__main__":
    # a little house: square + roof, two strokes
    xs = [10, 210, 210, 10, 10, 0, 110, 220]
    ys = [100, 100, 250, 250, 100, 110, 0, 110]
    up = [0, 0, 0, 0, 1, 0, 0, 1]
    print(show(rasterize(xs, ys, up)))

#!/usr/bin/env python3
"""Scribble Calc: stroke data, the symbol classes and the exact symbol rasteriser.

The rasteriser is the specification for the Mac's C code (app/recog.c): integer arithmetic only,
so the Python and Mac rasters are identical bit for bit.

Symbol raster (G x G = 24 x 24, 1 bit):
    bbox x0..x1, y0..y1 of all points of the symbol's strokes; w = x1 - x0, h = y1 - y0
    D  = max(w, h, minD)                 (minD keeps tiny symbols such as '.' small)
    sw = (w*B + D//2) // D, sh likewise  (B = 20: the content box)
    ox = (G - 1 - sw) // 2, oy likewise
    px = ox + ((x - x0)*B + D//2) // D,   py = oy + ((y - y0)*B + D//2) // D
    consecutive points of a stroke are joined with Bresenham lines (below); every visited
    pixel stamps a 2 x 2 brush (px..px+1, py..py+1), clipped to the grid. A one-point stroke
    stamps once.
"""
import gzip
import os
import re
import subprocess

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CALC = os.path.dirname(HERE)
DATA = os.path.join(CALC, "data")

# class order = token codes of app/dec.h (T_0..T_9, T_PLUS, T_MINUS, T_TIMES, T_DIV, T_EQ, T_LPAR, T_RPAR, T_POINT)
CLASSES = ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "+", "-", "x", "/", "=", "(", ")", "."]
SHOW = ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "+", "−", "×", "÷", "=", "(", ")", "."]
NCLS = len(CLASSES)
G = 24
B = 20


def bresenham(x0, y0, x1, y1):
    dx, sx = abs(x1 - x0), (1 if x0 < x1 else -1)
    dy, sy = -abs(y1 - y0), (1 if y0 < y1 else -1)
    err = dx + dy
    while True:
        yield x0, y0
        if x0 == x1 and y0 == y1:
            return
        e2 = 2 * err
        if e2 >= dy:
            err += dy
            x0 += sx
        if e2 <= dx:
            err += dx
            y0 += sy


def raster(strokes, minD):
    """strokes: list of int arrays (n, 2) of (x, y); returns uint8 (G, G) of 0/1."""
    pts = np.concatenate([np.asarray(s, dtype=np.int64).reshape(-1, 2) for s in strokes])
    x0, y0 = int(pts[:, 0].min()), int(pts[:, 1].min())
    w, h = int(pts[:, 0].max()) - x0, int(pts[:, 1].max()) - y0
    D = max(w, h, int(minD), 1)
    sw, sh = (w * B + D // 2) // D, (h * B + D // 2) // D
    ox, oy = (G - 1 - sw) // 2, (G - 1 - sh) // 2
    img = np.zeros((G + 1, G + 1), np.uint8)
    for s in strokes:
        s = np.asarray(s, dtype=np.int64).reshape(-1, 2)
        px = ox + ((s[:, 0] - x0) * B + D // 2) // D
        py = oy + ((s[:, 1] - y0) * B + D // 2) // D
        if len(s) == 1:
            img[py[0]:py[0] + 2, px[0]:px[0] + 2] = 1
            continue
        for i in range(len(s) - 1):
            for x, y in bresenham(int(px[i]), int(py[i]), int(px[i + 1]), int(py[i + 1])):
                img[y:y + 2, x:x + 2] = 1
    return img[:G, :G]


def raster_fast(strokes, minD):
    """Same result as raster(), vectorised for building training sets (checked against raster())."""
    pts = np.concatenate(strokes)
    x0, y0 = int(pts[:, 0].min()), int(pts[:, 1].min())
    w, h = int(pts[:, 0].max()) - x0, int(pts[:, 1].max()) - y0
    D = max(w, h, int(minD), 1)
    sw, sh = (w * B + D // 2) // D, (h * B + D // 2) // D
    ox, oy = (G - 1 - sw) // 2, (G - 1 - sh) // 2
    img = np.zeros((G + 1, G + 1), np.uint8)
    for s in strokes:
        px = ox + ((s[:, 0] - x0) * B + D // 2) // D
        py = oy + ((s[:, 1] - y0) * B + D // 2) // D
        if len(s) == 1:
            img[py[0]:py[0] + 2, px[0]:px[0] + 2] = 1
            continue
        for i in range(len(s) - 1):
            a, b, c, d = int(px[i]), int(py[i]), int(px[i + 1]), int(py[i + 1])
            if abs(c - a) <= 1 and abs(d - b) <= 1:
                img[b:b + 2, a:a + 2] = 1
                img[d:d + 2, c:c + 2] = 1
                continue
            for x, y in bresenham(a, b, c, d):
                img[y:y + 2, x:x + 2] = 1
    return img[:G, :G]


# ---------------------------------------------------------------- datasets (CC BY 4.0, UCI)

UJI_MAP = {c: c for c in "0123456789().-"}


def load_uji2(path=os.path.join(DATA, "uji2", "ujipenchars2.txt")):
    """UJI Pen Characters v2 (Prat et al., UCI, CC BY 4.0). Returns list of dicts:
    label (our class), strokes (float arrays, y down, ink units = 0.01 mm), split ('trn'/'tst'),
    writer, box height reference (the acquisition box guides). Also 'x' samples (lowercase x,
    used as extra multiplication signs)."""
    out = []
    cur = None
    with open(path, encoding="latin-1") as f:
        for line in f:
            if line.startswith("WORD"):
                parts = line.split()
                ch, wid = parts[1], parts[2]
                cur = None
                lab = UJI_MAP.get(ch)
                if ch == "x":
                    lab = "x"
                if lab is None:
                    continue
                m = re.match(r"(trn|tst)_(\w+?)_(W\d+)-(\d+)", wid)
                cur = dict(label=lab, strokes=[], split=m.group(1), writer=m.group(2) + m.group(3), src="uji2")
                out.append(cur)
            elif line.strip().startswith("POINTS") and cur is not None:
                nums = line.split("#")[1].split()
                a = np.array([int(v) for v in nums], dtype=np.float64).reshape(-1, 2)
                cur["strokes"].append(a)
    return out


def load_pendigits(which):
    """Pen-Based Recognition of Handwritten Digits, original version (Alpaydin & Alimoglu, UCI,
    CC BY 4.0). which = 'tra' (30 writers) or 'tes' (14 other writers). y is flipped to point down."""
    path = os.path.join(DATA, "pendigits", f"pendigits-orig.{which}.Z")
    raw = subprocess.run(["uncompress", "-c", path], capture_output=True, check=True).stdout.decode("latin-1")
    out = []
    cur = None
    stroke = None
    for line in raw.splitlines():
        t = line.strip()
        if t.startswith(".SEGMENT"):
            lab = t.split('"')[1]
            cur = dict(label=lab, strokes=[], split="trn" if which == "tra" else "tst", writer="pd", src="pendigits")
            out.append(cur)
            stroke = None
        elif t.startswith(".PEN_DOWN"):
            stroke = []
        elif t.startswith(".PEN_UP"):
            if cur is not None and stroke:
                cur["strokes"].append(np.array(stroke, dtype=np.float64))
            stroke = None
        elif stroke is not None and t and t[0].isdigit():
            x, y = t.split()[:2]
            stroke.append((float(x), -float(y)))
        elif t.startswith(".COMMENT") and cur is not None and not cur["strokes"] and stroke is None:
            cur["writer"] = "pd" + t.split()[1]
    return [o for o in out if o["strokes"]]


def ascii_art(img):
    return "\n".join("".join("#" if v else "." for v in row) for row in img)


if __name__ == "__main__":
    u = load_uji2()
    print("uji2", len(u), sorted(set(o["label"] for o in u)))
    p = load_pendigits("tra")
    print("pendigits tra", len(p))
    for o in (u[0], p[0], p[1]):
        s = [np.round(a).astype(np.int64) for a in o["strokes"]]
        h = max(a[:, 1].max() for a in s) - min(a[:, 1].min() for a in s)
        print(o["label"], o["writer"], o["split"])
        print(ascii_art(raster(s, h // 2)))

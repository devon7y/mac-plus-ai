#!/usr/bin/env python3
"""Rail Runner sprite masters: parser, previews, and helpers.

Master file format (runner/art/sprites.txt):
    ; comment lines start with ';'
    @ name ax ay          start a sprite; (ax, ay) = anchor pixel (feet centre / ground point)
    <rows>                one text row per pixel row; a blank line ends the sprite
Pixel characters:
    ' ' transparent        '#' black        '.' white (opaque)
    '+' 50% grey (black where (x + y) is odd, sprite-local)
    ':' 25% grey (black where x % 2 == 0 and y % 2 == 0)
Rows may differ in length; they are padded with transparent pixels on the right.

Usage:
    sprites.py preview [out.png]      contact sheet of every master at 3x on white and on gravel
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.join(os.path.dirname(HERE), "art")


class Sprite:
    def __init__(self, name, ax, ay, rows):
        self.name, self.ax, self.ay = name, ax, ay
        w = max((len(r) for r in rows), default=0)
        h = len(rows)
        self.ink = np.zeros((h, w), bool)      # True = black
        self.mask = np.zeros((h, w), bool)     # True = opaque
        for y, r in enumerate(rows):
            for x, c in enumerate(r):
                if c == " ":
                    continue
                self.mask[y, x] = True
                if c == "#":
                    self.ink[y, x] = True
                elif c == "+":
                    self.ink[y, x] = (x + y) & 1 == 1
                elif c == ":":
                    self.ink[y, x] = x % 2 == 0 and y % 2 == 0
                elif c == ".":
                    pass
                else:
                    raise SystemExit(f"sprite {name}: bad pixel {c!r}")

    @property
    def w(self):
        return self.ink.shape[1]

    @property
    def h(self):
        return self.ink.shape[0]


def load(path=os.path.join(ART, "sprites.txt")):
    sprites, cur, rows = {}, None, []

    def finish():
        if cur:
            name, ax, ay = cur
            while rows and not rows[-1].strip():
                rows.pop()
            sprites[name] = Sprite(name, ax, ay, rows[:])

    for line in open(path):
        line = line.rstrip("\n")
        if line.startswith(";"):
            continue
        if line.startswith("@"):
            finish()
            parts = line[1:].split()
            cur, rows = (parts[0], int(parts[1]), int(parts[2])), []
        elif cur is not None:
            if not line.strip():
                finish()
                cur, rows = None, []
            else:
                rows.append(line)
    finish()
    return sprites


def gravel(h, w):
    """The track's light gravel dither, for previews."""
    y, x = np.mgrid[0:h, 0:w]
    return ((x % 4 == 0) & (y % 2 == 0)) | ((x % 4 == 2) & (y % 2 == 1) & (y % 4 == 1))


def composite(bg, spr, x, y):
    """Draw sprite with its top-left at (x, y) onto bool image bg (True = black). Clipped."""
    h, w = bg.shape
    x0, y0 = max(x, 0), max(y, 0)
    x1, y1 = min(x + spr.w, w), min(y + spr.h, h)
    if x0 >= x1 or y0 >= y1:
        return
    m = spr.mask[y0 - y:y1 - y, x0 - x:x1 - x]
    k = spr.ink[y0 - y:y1 - y, x0 - x:x1 - x]
    reg = bg[y0:y1, x0:x1]
    reg[m] = k[m]


def to_image(bits, scale=1):
    img = Image.fromarray(np.where(bits, 0, 255).astype(np.uint8), "L")
    if scale != 1:
        img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
    return img


def preview(out=os.path.join(ART, "preview_sprites.png"), scale=3):
    sp = load()
    cells = []
    for s in sp.values():
        cw, ch = s.w + 8, s.h + 8
        a = np.zeros((ch, cw), bool)
        composite(a, s, 4, 4)
        b = gravel(ch, cw)
        composite(b, s, 4, 4)
        cells.append((s.name, a, b))
    # lay out in rows
    maxw = 1100
    x = y = 0
    rowh = 0
    placed = []
    for name, a, b in cells:
        cw = (a.shape[1] * 2 + 4) * scale
        ch = a.shape[0] * scale + 14
        if x + cw > maxw and x > 0:
            x, y, rowh = 0, y + rowh + 6, 0
        placed.append((name, a, b, x, y))
        x += cw + 10
        rowh = max(rowh, ch)
    H = y + rowh + 6
    img = Image.new("L", (maxw, H), 200)
    d = ImageDraw.Draw(img)
    for name, a, b, x, y in placed:
        img.paste(to_image(a, scale), (x, y + 12))
        img.paste(to_image(b, scale), (x + (a.shape[1] + 4) * scale, y + 12))
        d.text((x, y), name, fill=0)
    img.save(out)
    print(f"{len(cells)} sprites -> {out}")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "preview":
        preview(*sys.argv[2:])
    else:
        print(__doc__)

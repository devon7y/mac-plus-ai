#!/usr/bin/env python3
"""Contact sheet of icon files: each icon at 4x on white and on the gray desktop pattern, and at
1x on white, on gray and selected (inverted inside the mask, as the Finder shows a selected icon).

    sheet.py OUT.png [icon.txt ...]        default: every art/*.txt and private/*.txt

Drawn the way the Finder plots an ICN#: the mask is painted white, then the icon is ORed on top.
"""
import glob
import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from add_icon import load_icon  # noqa: E402

S = 4                                       # big scale
COLS = 3                                    # icons per row
CELL_W, CELL_H = 2 * 32 * S + 3 * 10 + 60, 32 * S + 34


def gray(x, y):                             # the System 6 desktop: 50 % gray (True = black)
    return (x + y) % 2 == 0


def bit(b, x, y):
    return b[y * 4 + x // 8] & (0x80 >> (x & 7))


def plot(px, ox, oy, icon, mask, scale, selected=False):
    """Mask painted white, icon ORed in; outside the mask the background stays."""
    for y in range(32):
        for x in range(32):
            if not bit(mask, x, y):
                continue
            v = 0 if bool(bit(icon, x, y)) != selected else 255
            for dy in range(scale):
                for dx in range(scale):
                    px[ox + x * scale + dx, oy + y * scale + dy] = v


def fill_gray(px, x0, y0, w, h):
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            px[x, y] = 0 if gray(x, y) else 255


def main():
    out = sys.argv[1]
    files = sys.argv[2:] or sorted(glob.glob(os.path.join(HERE, "art", "*.txt"))) + \
        sorted(glob.glob(os.path.join(HERE, "private", "*.txt")))
    rows = (len(files) + COLS - 1) // COLS
    img = Image.new("L", (COLS * CELL_W, rows * CELL_H), 255)
    px = img.load()
    d = ImageDraw.Draw(img)
    for i, f in enumerate(files):
        icon, mask = load_icon(f)
        x0, y0 = (i % COLS) * CELL_W, (i // COLS) * CELL_H
        d.text((x0 + 10, y0 + 6), os.path.basename(os.path.dirname(f)) + "/" + os.path.basename(f)[:-4], fill=0)
        top = y0 + 22
        plot(px, x0 + 10, top, icon, mask, S)                                  # 4x on white
        fill_gray(px, x0 + 20 + 32 * S, top - 4, 32 * S + 8, 32 * S + 8)
        plot(px, x0 + 24 + 32 * S, top, icon, mask, S)                         # 4x on gray
        sx = x0 + 2 * 32 * S + 40
        plot(px, sx, top, icon, mask, 1)                                       # 1x on white
        fill_gray(px, sx - 6, top + 40, 44, 88)
        plot(px, sx, top + 46, icon, mask, 1)                                  # 1x on gray
        plot(px, sx, top + 88, icon, mask, 1, selected=True)                   # 1x selected
        d.rectangle([x0, y0, x0 + CELL_W - 1, y0 + CELL_H - 1], outline=170)
    img.save(out)
    print(f"{len(files)} icons -> {out}")


if __name__ == "__main__":
    main()

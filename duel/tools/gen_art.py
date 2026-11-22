#!/usr/bin/env python3
"""Orbit Duel art: rotated ship sprites, thrust flames and the vector font, as C tables.

    gen_art.py <out_dir>     -> out_dir/art.c, art.h, art_preview.png (8x scale, for looking at)

All original. Ships are drawn the way a vector display would: 1-pixel lines between the corners of
each shape, here rasterised as every pixel whose centre lies within 0.55 px of a segment. The mask
(pixels painted black first) is the filled shape grown by one pixel, so a ship blots out the stars
behind it. 32 rotations of 11.25 degrees; sprites are 16x16, one 16-bit word per row, centred on
pixel (8, 8) (so a ship at integer position (x, y) is drawn with its cell's top-left at (x-8, y-8)).

Angles: 0 = pointing right, increasing counter-clockwise on the screen (index i = angle / 8 of the
game's 256-step angle).
"""
import math
import os
import struct
import sys
import zlib

N_ROT = 32
CELL = 16
C0 = 8.0            # the cell's centre in pixel coordinates (pixel centres at i + 0.5)

# Shapes in ship coordinates: x forward, y to the ship's left (math orientation); units = px.
NEEDLE = [(7.5, 0), (1.5, 1.6), (-3.5, 1.6), (-6.8, 4.6), (-5.2, 0), (-6.8, -4.6), (-3.5, -1.6), (1.5, -1.6)]
WEDGE = [(7.2, 0), (-5.0, 6.0), (-2.6, 2.2), (-2.6, -2.2), (-5.0, -6.0)]
SHIPS = [NEEDLE, WEDGE]
# flames: an open "V" behind the ship, two lengths for flicker. Its cell is centred FLAME_BACK px
# behind the ship's centre (the renderer adds flame_off[i]).
FLAME_BACK = 9.0
FLAMES = [[(-5.6, 1.4), (-9.6, 0), (-5.6, -1.4)], [(-5.6, 1.8), (-12.4, 0), (-5.6, -1.8)]]


def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    L = dx * dx + dy * dy
    t = 0 if L == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / L))
    qx, qy = ax + t * dx - px, ay + t * dy - py
    return math.hypot(qx, qy)


def inside(px, py, poly):
    c = False
    n = len(poly)
    for i in range(n):
        (x1, y1), (x2, y2) = poly[i], poly[(i + 1) % n]
        if (y1 > py) != (y2 > py) and px < (x2 - x1) * (py - y1) / (y2 - y1) + x1:
            c = not c
    return c


def rot_screen(pts, ang, cx, cy):
    """ship coords -> screen coords (y down) around (cx, cy)"""
    c, s = math.cos(ang), math.sin(ang)
    return [(cx + x * c - y * s, cy - (x * s + y * c)) for x, y in pts]


def raster(pts, closed, fill, width=0.55):
    """-> (white, filled) as sets of (col, row) in a CELL x CELL grid"""
    segs = [(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts) if closed else len(pts) - 1)]
    white, filled = set(), set()
    for r in range(CELL):
        for c in range(CELL):
            px, py = c + 0.5, r + 0.5
            if any(seg_dist(px, py, a[0], a[1], b[0], b[1]) <= width for a, b in segs):
                white.add((c, r))
            elif fill and inside(px, py, pts):
                filled.add((c, r))
    return white, filled


def words(pixels):
    out = []
    for r in range(CELL):
        w = 0
        for c in range(CELL):
            if (c, r) in pixels:
                w |= 0x8000 >> c
        out.append(w)
    return out


def grow(pixels):
    g = set(pixels)
    for (c, r) in pixels:
        for dc in (-1, 0, 1):
            for dr in (-1, 0, 1):
                if 0 <= c + dc < CELL and 0 <= r + dr < CELL:
                    g.add((c + dc, r + dr))
    return g


# ---- vector font: 4 x 6 grid (y down); strokes are polylines, "|" separates them ---------------
FONT = {
    'A': "0,6 0,2 2,0 4,2 4,6|0,4 4,4", 'B': "0,6 0,0 3,0 4,1 4,2 3,3 0,3|3,3 4,4 4,5 3,6 0,6",
    'C': "4,0 0,0 0,6 4,6", 'D': "0,0 2,0 4,2 4,4 2,6 0,6 0,0", 'E': "4,0 0,0 0,6 4,6|0,3 3,3",
    'F': "4,0 0,0 0,6|0,3 3,3", 'G': "4,1 4,0 0,0 0,6 4,6 4,3 2,3", 'H': "0,0 0,6|4,0 4,6|0,3 4,3",
    'I': "0,0 4,0|2,0 2,6|0,6 4,6", 'J': "4,0 4,6 2,6 0,4", 'K': "0,0 0,6|4,0 0,3 4,6",
    'L': "0,0 0,6 4,6", 'M': "0,6 0,0 2,2 4,0 4,6", 'N': "0,6 0,0 4,6 4,0",
    'O': "0,0 4,0 4,6 0,6 0,0", 'P': "0,6 0,0 4,0 4,3 0,3", 'Q': "0,0 4,0 4,4 2,6 0,6 0,0|2,4 4,6",
    'R': "0,6 0,0 4,0 4,3 0,3|1,3 4,6", 'S': "4,0 0,0 0,3 4,3 4,6 0,6", 'T': "0,0 4,0|2,0 2,6",
    'U': "0,0 0,6 4,6 4,0", 'V': "0,0 2,6 4,0", 'W': "0,0 0,6 2,4 4,6 4,0", 'X': "0,0 4,6|4,0 0,6",
    'Y': "0,0 2,2 4,0|2,2 2,6", 'Z': "0,0 4,0 0,6 4,6",
    '0': "0,0 4,0 4,6 0,6 0,0|4,0 0,6", '1': "1,1 2,0 2,6|1,6 3,6", '2': "0,0 4,0 4,3 0,3 0,6 4,6",
    '3': "0,0 4,0 4,6 0,6|1,3 4,3", '4': "0,0 0,3 4,3|4,0 4,6", '5': "4,0 0,0 0,3 3,3 4,4 4,6 0,6",
    '6': "4,0 0,0 0,6 4,6 4,3 0,3", '7': "0,0 4,0 4,1 1,6", '8': "0,0 4,0 4,6 0,6 0,0|0,3 4,3",
    '9': "4,3 0,3 0,0 4,0 4,6 0,6",
    '!': "2,0 2,4|2,6 2,6", '.': "2,6 2,6", ':': "2,1 2,1|2,5 2,5", '-': "1,3 3,3",
    '?': "0,1 0,0 4,0 4,3 2,3 2,4|2,6 2,6", "'": "2,0 2,2", ',': "2,5 2,6 1,7", '/': "4,0 0,6",
    '(': "3,0 1,2 1,4 3,6", ')': "1,0 3,2 3,4 1,6", '+': "2,1 2,5|0,3 4,3", '=': "0,2 4,2|0,4 4,4",
    '<': "4,0 0,3 4,6", '>': "0,0 4,3 0,6", ' ': "",
}


def font_bytes():
    """one byte per point: (x << 4) | y; 0xFF = pen up; 0xFE = end of glyph"""
    data, index = [], []
    for code in range(32, 96):
        ch = chr(code)
        index.append(len(data))
        strokes = FONT.get(ch)
        if strokes is None:
            strokes = FONT['?']
        for k, stroke in enumerate(s for s in strokes.split("|") if s):
            if k:
                data.append(0xFF)
            for p in stroke.split():
                x, y = (int(v) for v in p.split(","))
                data.append((x << 4) | y)
        data.append(0xFE)
    return data, index


def png(path, rows):
    h, w = len(rows), len(rows[0])
    raw = b"".join(b"\0" + bytes(row) for row in rows)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "build/gen"
    os.makedirs(out, exist_ok=True)
    ship_tab, flame_tab, flame_off = [], [], []
    preview = []
    for s, shape in enumerate(SHIPS):
        row = []
        for i in range(N_ROT):
            ang = i * 2 * math.pi / N_ROT
            pts = rot_screen(shape, ang, C0, C0)
            white, filled = raster(pts, True, True)
            mask = grow(white | filled)
            ship_tab.append((words(mask), words(white)))
            row.append((mask, white))
        preview.append(row)
    for f, shape in enumerate(FLAMES):
        row = []
        for i in range(N_ROT):
            ang = i * 2 * math.pi / N_ROT
            # flame cell centred FLAME_BACK px behind the ship centre, rounded to whole pixels
            ox = round(-FLAME_BACK * math.cos(ang))
            oy = round(FLAME_BACK * math.sin(ang))
            if f == 0:
                flame_off.append((ox, oy))
            pts = rot_screen(shape, ang, C0 - ox, C0 - oy)
            white, _ = raster(pts, False, False, 0.5)
            flame_tab.append(words(white))
            row.append((white, white))
        preview.append(row)
    fdata, findex = font_bytes()

    def arr(vals, per=12):
        return ",\n".join("    " + ", ".join(vals[i:i + per]) for i in range(0, len(vals), per))
    with open(os.path.join(out, "art.h"), "w") as h:
        h.write("/* generated by duel/tools/gen_art.py */\n#ifndef ART_H\n#define ART_H\n#include <stdint.h>\n"
                f"#define N_ROT {N_ROT}\n"
                "extern const uint16_t ship_spr[2][N_ROT][16][2];   /* [ship][rotation][row][mask, white] */\n"
                "extern const uint16_t flame_spr[2][N_ROT][16];     /* [length][rotation][row] white */\n"
                "extern const int8_t flame_off[N_ROT][2];           /* flame cell centre - ship centre (px) */\n"
                "extern const uint8_t font_data[];\n"
                "extern const uint16_t font_index[64];              /* ASCII 32..95 */\n"
                "#endif\n")
    with open(os.path.join(out, "art.c"), "w") as c:
        c.write('/* generated by duel/tools/gen_art.py */\n#include "art.h"\n')
        vals = []
        for mask, white in ship_tab:
            for r in range(CELL):
                vals += [f"0x{mask[r]:04X}", f"0x{white[r]:04X}"]
        c.write("const uint16_t ship_spr[2][N_ROT][16][2] = {\n" + arr(vals, 16) + "\n};\n")
        vals = [f"0x{w:04X}" for fl in flame_tab for w in fl]
        c.write("const uint16_t flame_spr[2][N_ROT][16] = {\n" + arr(vals, 16) + "\n};\n")
        vals = [str(v) for o in flame_off for v in o]
        c.write("const int8_t flame_off[N_ROT][2] = {\n" + arr(vals, 16) + "\n};\n")
        c.write("const uint8_t font_data[] = {\n" + arr([f"0x{v:02X}" for v in fdata], 16) + "\n};\n")
        c.write("const uint16_t font_index[64] = {\n" + arr([str(v) for v in findex], 16) + "\n};\n")
    # preview: 4 rows (needle, wedge, flame short, flame long) of 32 cells, 8x, grey = mask only
    S = 6
    rows = [[40] * (N_ROT * (CELL + 2) * S) for _ in range(len(preview) * (CELL + 2) * S)]
    for pr, row in enumerate(preview):
        for i, (mask, white) in enumerate(row):
            for r in range(CELL):
                for col in range(CELL):
                    v = 255 if (col, r) in white else 110 if (col, r) in mask else 0
                    for yy in range(S):
                        for xx in range(S):
                            rows[(pr * (CELL + 2) + r) * S + yy][(i * (CELL + 2) + col) * S + xx] = v
    png(os.path.join(out, "art_preview.png"), rows)
    print(f"art: {len(ship_tab)} ship cells, {len(flame_tab)} flame cells, font {len(fdata)} bytes -> {out}")


if __name__ == "__main__":
    main()

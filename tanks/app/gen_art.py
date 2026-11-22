#!/usr/bin/env python3
"""Tank Duel art: every bitmap of the game, drawn from geometric models (original designs).

    gen_art.py OUTDIR [--preview sheet.png]

Writes OUTDIR/art.c and art.h:
  hulls     4 designs (player, rusher, sniper, careful) x 2 tread phases x 32 angles, 16x16
  turrets   4 designs x 32 angles, 24x24 (white halo so they show on dark hulls)
  shell, spark (2 frames), explosion (6 frames, 32x32), crosshair, shield ring
  wall and floor tiles (16x16, chosen by the neighbouring walls), the title logo
Sprite data: per 16-pixel strip, per row, a mask word then an image word (1 = black).
"""
import math
import os
import sys

import numpy as np

N_ANG = 32
SS = 4                     # supersampling per axis

# ----------------------------------------------------------------------- materials
WHITE, BLACK, CHECK, DARK, LIGHT = 1, 2, 3, 4, 5


def shade(mat, px, py):
    if mat == WHITE:
        return 0
    if mat == BLACK:
        return 1
    if mat == CHECK:
        return (px + py) & 1
    if mat == DARK:
        return 0 if ((px & 1) == 0 and (py & 1) == 0) else 1
    if mat == LIGHT:
        return 1 if ((px & 1) == 0 and (py & 1) == 0) else 0
    return 0


# ----------------------------------------------------------------------- models (angle 0 = facing +x)
BODY = {0: WHITE, 1: BLACK, 2: CHECK, 3: DARK}


def hull_model(design, phase):
    body = BODY[design]

    def f(x, y):
        ay = abs(y)
        if -7.0 <= x <= 7.0 and 3.0 <= ay <= 6.6:          # treads
            if ay <= 3.6:
                return BLACK                                # seam between tread and body
            mark = ((x + 7.0 + phase * 1.5) % 3.0) < 1.0
            if design == 0:
                return BLACK if mark else WHITE             # player: light treads with dark cleats
            return WHITE if (mark and 4.4 <= ay <= 5.6) else BLACK
        if design == 3:                                     # careful: wider armour, black front plate
            if -6.2 <= x <= 6.2 and ay <= 3.0:
                return BLACK if x > 4.4 else body
            return None
        if -5.4 <= x <= 5.4 and ay <= 3.0:
            if design == 1 and x > 3.6:
                return WHITE                                # rusher: white nose
            if design == 2 and x < -3.6:
                return WHITE                                # sniper: white tail
            return body
        return None
    return f


TURRET = {  # dome radius, barrel length, barrel half width
    0: (3.4, 10.0, 1.15),
    1: (3.2, 8.2, 1.45),
    2: (2.9, 11.8, 0.9),
    3: (3.9, 9.2, 1.15),
}


def turret_model(design):
    r, ln, hw = TURRET[design]

    def f(x, y):
        if 1.0 <= x <= ln and abs(y) <= hw:
            return BLACK
        d = math.hypot(x, y)
        if d <= r:
            if design == 0:
                return WHITE                     # the player's turret: white dome
            if design == 2 and d <= 1.1:
                return WHITE                     # sniper: a scope dot
            if design == 3 and 1.6 <= d <= 2.4:
                return WHITE                     # careful: a ring
            return BLACK                         # enemies: black domes
        return None
    return f


def raster(model, size, angle, cx=None, cy=None, thr=0.45):
    """rotate the model by angle (radians), sample it: (opaque bool, value 0/1) arrays"""
    cx = size / 2 if cx is None else cx
    cy = size / 2 if cy is None else cy
    ca, sa = math.cos(angle), math.sin(angle)
    op = np.zeros((size, size), bool)
    val = np.zeros((size, size), np.uint8)
    for py in range(size):
        for px in range(size):
            mats = []
            for sy in range(SS):
                for sx in range(SS):
                    dx = px + (sx + 0.5) / SS - cx
                    dy = py + (sy + 0.5) / SS - cy
                    mx = ca * dx + sa * dy
                    my = -sa * dx + ca * dy
                    m = model(mx, my)
                    if m is not None:
                        mats.append(m)
            if len(mats) >= thr * SS * SS:
                op[py, px] = True
                # the most common material, thin dark features first
                best = max(set(mats), key=lambda m: (mats.count(m) + (1 if m == BLACK else 0)))
                val[py, px] = shade(best, px, py)
    return op, val


def outline(op, val):
    """opaque pixels on the shape's edge become black"""
    h, w = op.shape
    v = val.copy()
    for y in range(h):
        for x in range(w):
            if not op[y, x]:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if nx < 0 or ny < 0 or nx >= w or ny >= h or not op[ny, nx]:
                    v[y, x] = 1
                    break
    return v


def dilate(op):
    o = op.copy()
    o[1:, :] |= op[:-1, :]
    o[:-1, :] |= op[1:, :]
    o[:, 1:] |= op[:, :-1]
    o[:, :-1] |= op[:, 1:]
    return o


def to_words(mask, img):
    """(h, w) arrays -> list of words: per 16-px strip, per row: mask, image"""
    h, w = mask.shape
    ns = (w + 15) // 16
    out = []
    for s in range(ns):
        for y in range(h):
            mw = iw = 0
            for b in range(16):
                x = s * 16 + b
                if x < w:
                    if mask[y, x]:
                        mw |= 0x8000 >> b
                    if img[y, x] and mask[y, x]:
                        iw |= 0x8000 >> b
            out += [mw, iw]
    return out, ns


class Sheet:
    def __init__(self):
        self.data = []
        self.sprites = []          # (name, w, h, ox, oy, nstrip, offset)
        self.preview = []

    def add(self, name, mask, img, ox, oy):
        ys, xs = np.nonzero(mask)            # crop to the mask's bounding box (less to draw and copy)
        if len(ys):
            y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
            mask, img = mask[y0:y1, x0:x1], img[y0:y1, x0:x1]
            ox, oy = ox - x0, oy - y0
        words, ns = to_words(mask, img)
        h, w = mask.shape
        self.sprites.append((name, w, h, ox, oy, ns, len(self.data)))
        self.data += words
        self.preview.append((mask, img))
        return len(self.sprites) - 1


def make_sprites(sh):
    idx = {}
    for design in range(4):
        for phase in range(2):
            for k in range(N_ANG):
                a = 2 * math.pi * k / N_ANG
                op, val = raster(hull_model(design, phase), 16, a)
                val = outline(op, val)
                i = sh.add(f"hull{design}_{phase}_{k}", op, val, 8, 8)
                idx.setdefault("hull", i if (design, phase, k) == (0, 0, 0) else idx.get("hull"))
    for design in range(4):
        for k in range(N_ANG):
            a = 2 * math.pi * k / N_ANG
            op, val = raster(turret_model(design), 24, a)
            val = outline(op, val)
            m = dilate(op)
            i = sh.add(f"tur{design}_{k}", m, val & op, 12, 12)
            if (design, k) == (0, 0):
                idx["tur"] = i
    # shell
    yy, xx = np.mgrid[0:7, 0:7]
    d = np.hypot(xx - 3, yy - 3)
    op = d <= 2.3
    idx["shell"] = sh.add("shell", dilate(op), op.astype(np.uint8), 3, 3)
    # sparks
    sp0 = np.zeros((9, 9), bool)
    sp0[4, 1:8] = True
    sp0[1:8, 4] = True
    sp0[4, 4] = False
    sp1 = np.zeros((9, 9), bool)
    for t in range(1, 5):
        for sx, sy in ((1, 1), (1, -1), (-1, 1), (-1, -1)):
            if t in (1, 3, 4):
                sp1[4 + sy * t, 4 + sx * t] = True
    idx["spark"] = sh.add("spark0", dilate(sp0), sp0.astype(np.uint8), 4, 4)
    sh.add("spark1", dilate(sp1), sp1.astype(np.uint8), 4, 4)
    # explosion: 6 frames
    rng = np.random.default_rng(7)
    spikes = rng.random(16)
    for f in range(6):
        size = 32
        R = [4.5, 8.0, 11.0, 13.0, 14.0, 14.5][f]
        op = np.zeros((size, size), bool)
        img = np.zeros((size, size), np.uint8)
        for y in range(size):
            for x in range(size):
                dx, dy = x + 0.5 - 16, y + 0.5 - 16
                d = math.hypot(dx, dy)
                th = math.atan2(dy, dx)
                k = (th / (2 * math.pi) * 16) % 16
                s = spikes[int(k)] * (1 - (k % 1)) + spikes[int(k + 1) % 16] * (k % 1)
                rr = R * (0.65 + 0.5 * s)
                if f <= 3:
                    if d <= rr:
                        op[y, x] = True
                        core = rr * [0.75, 0.6, 0.5, 0.3][f]
                        if d <= core:
                            img[y, x] = 0
                        elif d <= rr - 1.6:
                            img[y, x] = (x + y) & 1 if f >= 2 else 0
                        else:
                            img[y, x] = 1
                else:   # smoke puffs breaking up
                    if d <= rr and rng.random() < (0.55 if f == 4 else 0.3):
                        op[y, x] = True
                        img[y, x] = 1 if ((x * 7 + y * 3) % 5) < 2 else 0
        if f <= 3:
            # debris: a few black dots flying out
            for j in range(10):
                th = 2 * math.pi * j / 10 + 0.3
                dd = R + 1.5 + f * 0.5
                x, y = int(16 + dd * math.cos(th)), int(16 + dd * math.sin(th))
                if 0 <= x < 32 and 0 <= y < 32:
                    op[y, x] = True
                    img[y, x] = 1
        i = sh.add(f"boom{f}", op, img, 16, 16)
        if f == 0:
            idx["boom"] = i
    # crosshair
    op = np.zeros((13, 13), bool)
    for y in range(13):
        for x in range(13):
            d = math.hypot(x - 6, y - 6)
            if abs(d - 4.2) < 0.55:
                op[y, x] = True
    op[6, 0:3] = op[6, 10:13] = op[0:3, 6] = op[10:13, 6] = True
    op[6, 6] = True
    idx["cross"] = sh.add("cross", dilate(op), op.astype(np.uint8), 6, 6)
    # shield ring (blinks around a respawned tank)
    op = np.zeros((22, 22), bool)
    for y in range(22):
        for x in range(22):
            d = math.hypot(x + 0.5 - 11, y + 0.5 - 11)
            if abs(d - 10) < 0.6 and ((x + y) % 3) != 0:
                op[y, x] = True
    idx["ring"] = sh.add("ring", op, op.astype(np.uint8), 11, 11)
    return idx


# ----------------------------------------------------------------------- tiles
def wall_tile(up, right, down, left):
    """black block with a white inner line along the edges that face the floor"""
    t = np.ones((16, 16), np.uint8)
    x0 = 2 if left else 0
    x1 = 13 if right else 15
    y0 = 2 if up else 0
    y1 = 13 if down else 15
    if up:
        t[2, x0:x1 + 1] = 0
    if down:
        t[13, x0:x1 + 1] = 0
    if left:
        t[y0:y1 + 1, 2] = 0
    if right:
        t[y0:y1 + 1, 13] = 0
    return t


def floor_tile(wl, wu, wul):
    """white floor with a dot, and a dithered shadow next to walls on the left / above"""
    t = np.zeros((16, 16), np.uint8)
    t[8, 8] = 1
    for y in range(16):
        for x in range(16):
            sh = (wl and x < 3) or (wu and y < 3) or (wul and x < 3 and y < 3)
            if sh and (x + y) % 2 == 0:
                t[y, x] = 1
    return t


def tile_words(t):
    return [int("".join(str(int(v)) for v in row), 2) for row in t]


# ----------------------------------------------------------------------- logo
FONT = {
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "N": ["#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"],
    "K": ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    "D": ["####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."],
    "U": ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    " ": [".....", ".....", ".....", ".....", ".....", ".....", "....."],
}


def logo(text="TANK DUEL", scale=5):
    w = len(text) * 6 * scale + 2 * scale
    h = 7 * scale + 2 * scale
    w = (w + 15) // 16 * 16
    img = np.zeros((h, w), np.uint8)
    face = np.zeros((h, w), bool)
    for ci, ch in enumerate(text):
        g = FONT[ch]
        for gy, row in enumerate(g):
            for gx, c in enumerate(row):
                if c == "#":
                    x0 = scale + ci * 6 * scale + gx * scale
                    y0 = scale // 2 + gy * scale
                    face[y0:y0 + scale, x0:x0 + scale] = True
    # extruded shadow down-right (dithered), black outline, white face with a hatch band
    sh = np.zeros_like(face)
    for k in range(1, scale):
        sh[k:, k:] |= face[:-k, :-k]
    img[sh & ~face] = 0
    for y in range(h):
        for x in range(w):
            if sh[y, x] and not face[y, x]:
                img[y, x] = 1 if (x + y) % 2 == 0 else 0
    edge = dilate(face) & ~face
    img[edge] = 1
    for y in range(h):
        for x in range(w):
            if face[y, x]:
                img[y, x] = 1 if (y % 4 == 3 and y > h // 2) else 0
    return img


# ----------------------------------------------------------------------- output
def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    sh = Sheet()
    idx = make_sprites(sh)
    walls = [tile_words(wall_tile(bool(m & 1), bool(m & 2), bool(m & 4), bool(m & 8))) for m in range(16)]
    floors = [tile_words(floor_tile(bool(m & 1), bool(m & 2), bool(m & 4))) for m in range(8)]
    lg = logo()
    lh, lw = lg.shape
    c = ["/* Generated by gen_art.py. Do not edit. */", '#include "art.h"', ""]
    c.append(f"static const unsigned short spr_data[{len(sh.data)}] = {{")
    for i in range(0, len(sh.data), 16):
        c.append("    " + ", ".join(f"0x{v:04x}" for v in sh.data[i:i + 16]) + ",")
    c.append("};")
    c.append(f"const Spr SPR[{len(sh.sprites)}] = {{")
    for (name, w, h, ox, oy, ns, off) in sh.sprites:
        c.append(f"    {{{w}, {h}, {ox}, {oy}, {ns}, spr_data + {off}}},   /* {name} */")
    c.append("};")
    c.append("const unsigned short WALL_TILE[16][16] = {")
    for t in walls:
        c.append("    {" + ", ".join(f"0x{v:04x}" for v in t) + "},")
    c.append("};")
    c.append("const unsigned short FLOOR_TILE[8][16] = {")
    for t in floors:
        c.append("    {" + ", ".join(f"0x{v:04x}" for v in t) + "},")
    c.append("};")
    lb = []
    for y in range(lh):
        for x in range(0, lw, 8):
            lb.append(int("".join(str(int(v)) for v in lg[y, x:x + 8]), 2))
    c.append(f"const unsigned char LOGO[{len(lb)}] = {{")
    for i in range(0, len(lb), 24):
        c.append("    " + ", ".join(str(v) for v in lb[i:i + 24]) + ",")
    c.append("};")
    h = ["/* Generated by gen_art.py. Do not edit. */", "#ifndef ART_H", "#define ART_H",
         "typedef struct { short w, h, ox, oy, nstrip; const unsigned short *d; } Spr;",
         f"extern const Spr SPR[{len(sh.sprites)}];",
         "#define N_ANG 32"]
    for k, v in idx.items():
        h.append(f"#define SPR_{k.upper()} {v}")
    h += ["#define SPR_HULL_AT(design, phase, ang) (SPR_HULL + ((design) * 2 + (phase)) * N_ANG + (ang))",
          "#define SPR_TUR_AT(design, ang) (SPR_TUR + (design) * N_ANG + (ang))",
          "extern const unsigned short WALL_TILE[16][16];   /* index: up 1, right 2, down 4, left 8 = floor there */",
          "extern const unsigned short FLOOR_TILE[8][16];   /* index: wall left 1, up 2, up-left 4 */",
          f"#define LOGO_W {lw}", f"#define LOGO_H {lh}",
          "extern const unsigned char LOGO[];", "#endif", ""]
    open(os.path.join(out, "art.c"), "w").write("\n".join(c) + "\n")
    open(os.path.join(out, "art.h"), "w").write("\n".join(h))
    print(f"art: {len(sh.sprites)} sprites, {len(sh.data) * 2} bytes; logo {lw}x{lh}")
    if "--preview" in sys.argv:
        from PIL import Image
        path = sys.argv[sys.argv.index("--preview") + 1]
        W = 34 * 32
        rows = []
        cur = []
        for mask, img in sh.preview:
            cur.append((mask, img))
            if len(cur) == 32:
                rows.append(cur)
                cur = []
        if cur:
            rows.append(cur)
        Hh = len(rows) * 34 + 2 * 18 + lh + 10
        canvas = np.full((Hh, W), 200, np.uint8)
        for r, row in enumerate(rows):
            for k, (mask, img) in enumerate(row):
                hh, ww = mask.shape
                y0, x0 = r * 34 + 1, k * 34 + 1
                for y in range(hh):
                    for x in range(ww):
                        if mask[y, x]:
                            canvas[y0 + y, x0 + x] = 0 if img[y, x] else 255
        y0 = len(rows) * 34 + 2
        for k, t in enumerate(walls + floors):
            for y in range(16):
                for x in range(16):
                    canvas[y0 + y, k * 18 + x] = 0 if (t[y] >> (15 - x)) & 1 else 255
        y0 += 36
        for y in range(lh):
            for x in range(lw):
                canvas[y0 + y, x] = 0 if lg[y, x] else 255
        Image.fromarray(canvas).resize((W * 2, Hh * 2), Image.NEAREST).save(path)
        print("preview", path)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Blockworld mobs: the pictures (original designs) and the table of kinds, written as C.

    gen_mobs.py OUTDIR [--sheet sheet.png]

Pictures: '#' black, 'o' white, '.' clear. Each kind has a side view facing right (two walking
frames; mirrored when it faces left) and a front view (also used from behind). The sizes are in
blocks (the sprite's width and height on screen for the side view; the front view keeps the
height and takes its width from its own aspect).
"""
import os
import sys

# ---- passive -----------------------------------------------------------------------------------
YAK_SIDE = [
    "..............#..#..",
    "..............####..",
    "...###########o###..",
    "..#oooo#ooo##oo#oo#.",
    ".#oooooooooooo#o##o#",
    "#ooo##oooooo#oo#ooo#",
    "#oooooooo##oooo####.",
    "#o##ooooooooooo#....",
    "#ooooooo##oooo#.....",
    ".##############.....",
    "..#o#.#o#.#o#.#o#...",
    "..#o#.#o#.#o#.#o#...",
    "..###.###.###.###...",
]
YAK_SIDE2 = YAK_SIDE[:10] + [
    "...#o#.#o#.#o#.#o#..",
    "..#o#.#o#.#o#.#o#...",
    "..##..##..###.###...",
]
YAK_FRONT = [
    "#..........#",
    "##........##",
    ".##########.",
    "..#oooooo#..",
    ".#o##oo##o#.",
    ".#oooooooo#.",
    ".#ooo##ooo#.",
    ".#oo#oo#oo#.",
    "..#oooooo#..",
    "..########..",
    "..#o#..#o#..",
    "..#o#..#o#..",
    "..###..###..",
]
BOAR_SIDE = [
    "................",
    "..........#.#...",
    "...########o##..",
    "..#oooooooo#oo#.",
    ".#oooooooooo#oo#",
    "#oooooooooooo#o#",
    "#ooooooooooo#oo#",
    ".#oooooooooo####",
    "..###########...",
    "..#o#.....#o#...",
    "..###.....###...",
]
BOAR_SIDE2 = BOAR_SIDE[:9] + [
    "...#o#...#o#....",
    "...###...###....",
]
BOAR_FRONT = [
    "..#......#..",
    "..##....##..",
    ".#oo####oo#.",
    "#oooooooooo#",
    "#o#oooooo#o#",
    "#oooo##oooo#",
    "#ooo#oo#ooo#",
    ".#oo#..#oo#.",
    "..########..",
    "..#o#..#o#..",
    "..###..###..",
]
RAM_SIDE = [
    "............##..",
    "...........#oo#.",
    "..#########o#o#.",
    ".#o#o#o#o#o##oo#",
    "#o#o#o#o#o#o#o##",
    "#oo#o#o#o#o##oo#",
    "#o#o#o#o#o#o####",
    ".#o#o#o#o#o#....",
    "..##########....",
    "..#o#....#o#....",
    "..#o#....#o#....",
    "..###....###....",
]
RAM_SIDE2 = RAM_SIDE[:9] + [
    "...#o#..#o#.....",
    "..#o#....#o#....",
    "..##.....##.....",
]
RAM_FRONT = [
    ".##......##.",
    "#oo#....#oo#",
    "#o#.####.#o#",
    ".#.#oooo#.#.",
    "..#o#oo#o#..",
    ".#o#o##o#o#.",
    "#o#o#oo#o#o#",
    "#oo#o##o#oo#",
    ".#o#o#o#o#..",
    "..########..",
    "..#o#..#o#..",
    "..###..###..",
]
FOWL_SIDE = [
    "........##..",
    ".......#oo#.",
    ".......#o###",
    ".#.....#oo#.",
    "#o#####ooo#.",
    "#oooooooooo#",
    ".#ooooo##oo#",
    "..#oooooooo#",
    "...#######..",
    "....#..#....",
    "...##.##....",
]
FOWL_SIDE2 = FOWL_SIDE[:9] + [
    ".....#.#....",
    "....##.##...",
]
FOWL_FRONT = [
    "...####...",
    "..#oooo#..",
    "..#o##o#..",
    "..#o##o#..",
    ".#oo##oo#.",
    "#oooooooo#",
    "#oooooooo#",
    ".#oooooo#.",
    "..######..",
    "...#..#...",
    "..##..##..",
]
# ---- hostile -----------------------------------------------------------------------------------
SHAMBLER_SIDE = [
    "...#####......",
    "..#ooooo#.....",
    "..#oo#o#o#....",
    "..#ooooo#.....",
    "...#o#o#......",
    "..###########.",
    ".#ooooooooooo#",
    ".#o#ooo######.",
    ".#o#ooo#......",
    ".#ooooo#......",
    ".#oo#oo#......",
    "..#####.......",
    "..#o#o#.......",
    "..#o#o#.......",
    "..#o#o#.......",
    "..#o#o#.......",
    "..##.##.......",
]
SHAMBLER_SIDE2 = SHAMBLER_SIDE[:12] + [
    "..#o##o#......",
    ".#o#..#o#.....",
    ".#o#..#o#.....",
    "#o#....#o#....",
    "##......##....",
]
SHAMBLER_FRONT = [
    "...######...",
    "..#oooooo#..",
    "..#o##o#o#..",
    "..#oooooo#..",
    "...#o##o#...",
    ".##########.",
    "#oo#oooo#oo#",
    "#o##oooo##o#",
    "#o##o##o##o#",
    "#o##oooo##o#",
    "##.#oooo#.##",
    "...######...",
    "...#o##o#...",
    "...#o##o#...",
    "...#o##o#...",
    "...#o##o#...",
    "...##..##...",
]
ARCHER_SIDE = [
    "...#####....",
    "..#o#o#o#...",
    "..#ooooo#.#.",
    "...#o#o#.#o#",
    "....###..#.#",
    "...#o#o#.#.#",
    "..#o###o##.#",
    "..#o#o####.#",
    "...#o#o#.#.#",
    "....#o#..#.#",
    "...#o#o#..#.",
    "...#o.o#....",
    "...#o.o#....",
    "...#o.o#....",
    "...#o.o#....",
    "...##.##....",
]
ARCHER_SIDE2 = ARCHER_SIDE[:11] + [
    "...#o#o#....",
    "..#o#.#o#...",
    "..#o#.#o#...",
    ".#o#...#o#..",
    ".##.....##..",
]
ARCHER_FRONT = [
    "...######...",
    "..#oooooo#..",
    "..#o##o#o#..",
    "..#oooooo#..",
    "...#o##o#...",
    "....####....",
    "..#o#oo#o#..",
    ".#o##oo##o#.",
    ".#o#o##o#o#.",
    ".#o#oooo#o#.",
    ".##.#oo#.##.",
    "....####....",
    "...#o..o#...",
    "...#o..o#...",
    "...#o..o#...",
    "...##..##...",
]
CRAWLER_SIDE = [
    "........................",
    ".........######.........",
    "...#....#oooooo#....#...",
    "..#.#..#oo#oo#oo#..#.#..",
    ".#...##oooooooooo##...#.",
    "#...#.#oo#oooo#oo#.#...#",
    "...#...#oooooooo#...#...",
    "..#.....########.....#..",
    ".#......#......#......#.",
    "#......#........#......#",
]
CRAWLER_SIDE2 = CRAWLER_SIDE[:2] + [
    "........#oooooo#........",
    "...#..##oo#oo#oo##..#...",
    "..#.##.oooooooooo.##.#..",
    ".#...#.#oo#oooo#oo.#...#",
    "#...#..#oooooooo#...#...",
    "...#....########.....#..",
    "..#.....#......#......#.",
    ".#.....#........#......#",
]
CRAWLER_FRONT = [
    "....................",
    ".......######.......",
    "..#...#oooooo#...#..",
    ".#.#.#o##oo##o#.#.#.",
    "#...##o##oo##o##...#",
    "...#.#oooooooo#.#...",
    "..#..#o#oooo#o#..#..",
    ".#....########....#.",
    "#....#........#....#",
]
SIZZLER_SIDE = [
    "..######..",
    ".#o#oo#o#.",
    "#oooooooo#",
    "#o##oo##o#",
    "#oooooooo#",
    ".#oo##oo#.",
    "..#oooo#..",
    "..#o##o#..",
    "..#oooo#..",
    "..#o#oo#..",
    "..#oooo#..",
    "..#oo#o#..",
    "..#oooo#..",
    ".#oooooo#.",
    ".#o#..#o#.",
    ".###..###.",
]
SIZZLER_SIDE2 = SIZZLER_SIDE[:13] + [
    ".#oooooo#.",
    "#o#....#o#",
    "###....###",
]
SIZZLER_FRONT = SIZZLER_SIDE

# kind: name, hp, half width, height (blocks), walk speed (1/256 block per tick), hostile,
#       (unused), side pictures, front picture, drops [(item name, min, max)]. The sprite is as tall
#       as the body; its width follows the picture (square texels).
KINDS = [
    ("yak", 10, 0.45, 1.3, 30, 0, 1.5, (YAK_SIDE, YAK_SIDE2), YAK_FRONT, [("hide", 0, 2), ("raw meat", 1, 3)]),
    ("boar", 10, 0.45, 0.9, 30, 0, 1.2, (BOAR_SIDE, BOAR_SIDE2), BOAR_FRONT, [("raw meat", 1, 3)]),
    ("ram", 8, 0.45, 1.2, 30, 0, 1.3, (RAM_SIDE, RAM_SIDE2), RAM_FRONT, [("wool", 1, 2), ("raw meat", 1, 2)]),
    ("fowl", 4, 0.25, 0.7, 30, 0, 0.7, (FOWL_SIDE, FOWL_SIDE2), FOWL_FRONT, [("feather", 0, 2), ("raw meat", 1, 1)]),
    ("shambler", 20, 0.3, 1.9, 26, 1, 1.0, (SHAMBLER_SIDE, SHAMBLER_SIDE2), SHAMBLER_FRONT, [("raw meat", 0, 2)]),
    ("archer", 20, 0.3, 1.9, 32, 1, 0.8, (ARCHER_SIDE, ARCHER_SIDE2), ARCHER_FRONT, [("arrow", 0, 2), ("stick", 0, 1)]),
    ("crawler", 16, 0.6, 0.8, 44, 1, 1.6, (CRAWLER_SIDE, CRAWLER_SIDE2), CRAWLER_FRONT, [("string", 0, 2)]),
    ("sizzler", 20, 0.3, 1.6, 30, 1, 0.65, (SIZZLER_SIDE, SIZZLER_SIDE2), SIZZLER_FRONT, [("flint", 0, 2)]),
]


def check():
    for k in KINDS:
        for pic in (k[7][0], k[7][1], k[8]):
            w = len(pic[0])
            for row in pic:
                assert len(row) == w and set(row) <= set("#o."), (k[0], row)
        assert len(k[7][0]) == len(k[7][1]) and len(k[7][0][0]) == len(k[7][1][0]), k[0]


def sheet(path):
    from PIL import Image
    S = 6
    W = sum(max(len(k[7][0][0]), len(k[8][0])) * 3 + 6 for k in KINDS)
    img = Image.new("L", (W * S, 24 * S), 160)
    x0 = 0
    for k in KINDS:
        for j, pic in enumerate((k[7][0], k[7][1], k[8])):
            for y, row in enumerate(pic):
                for x, ch in enumerate(row):
                    if ch != ".":
                        for dy in range(S):
                            for dx in range(S):
                                img.putpixel(((x0 + x) * S + dx, (y + 2) * S + dy), 0 if ch == "#" else 255)
            x0 += len(pic[0]) + 2
        x0 += 2
    img.save(path)


def write(out):
    import gen_items as G
    names = {n: i for i, n in enumerate(G.names) if n}
    h = ["/* generated by gen_mobs.py */", "#ifndef BW_MOBS_GEN_H", "#define BW_MOBS_GEN_H",
         f"#define NKINDS {len(KINDS)}",
         "typedef struct MobKind {",
         "    const char *name;",
         "    short hp, hw, ht, speed, hostile;           /* hw, ht: 8.8 blocks */",
         "    short sw, sh, fw;                           /* sprite: side width, height, front width (8.8) */",
         "    const char *const *side[2], *const *front;",
         "    short pw, ph, fpw, fph;                     /* picture sizes (texels) */",
         "    unsigned char drop[2][3];                   /* item, min, max */",
         "} MobKind;",
         "extern const MobKind mob_kind[NKINDS];",
         "extern const unsigned char mob_atan[65];       /* atan(i / 64) in 1/1024 turns */"]
    for i, k in enumerate(KINDS):
        h.append(f"#define MK_{k[0].upper()} {i}")
    h.append("#endif")
    c = ["/* generated by gen_mobs.py */", '#include "mobs_gen.h"']
    for k in KINDS:
        for nm, pic in (("s0", k[7][0]), ("s1", k[7][1]), ("f", k[8])):
            c.append(f"static const char *const {k[0]}_{nm}[{len(pic)}] = {{")
            c += [f'    "{row}",' for row in pic]
            c.append("};")
    import math
    c.append("const unsigned char mob_atan[65] = {" + ", ".join(
        str(round(math.atan(i / 64) * 1024 / (2 * math.pi))) for i in range(65)) + "};")
    c.append("const MobKind mob_kind[NKINDS] = {")
    for k in KINDS:
        name, hp, hw, ht, speed, hostile, sw, (s0, s1), fr, drops = k
        sh = ht                                      # square texels: the picture's aspect
        sw = sh * len(s0[0]) / len(s0)
        fw = sh * len(fr[0]) / len(fr)
        d = []
        for j in range(2):
            if j < len(drops):
                n, lo, hi = drops[j]
                d.append(f"{{{names[n]}, {lo}, {hi}}}")
            else:
                d.append("{0, 0, 0}")
        c.append(f'    {{"{name}", {hp}, {round(hw * 256)}, {round(ht * 256)}, {speed}, {hostile}, '
                 f'{round(sw * 256)}, {round(sh * 256)}, {round(fw * 256)}, {{{name}_s0, {name}_s1}}, {name}_f, '
                 f'{len(s0[0])}, {len(s0)}, {len(fr[0])}, {len(fr)}, {{{", ".join(d)}}}}},')
    c.append("};")
    open(os.path.join(out, "mobs_gen.h"), "w").write("\n".join(h) + "\n")
    open(os.path.join(out, "mobs_gen.c"), "w").write("\n".join(c) + "\n")


if __name__ == "__main__":
    check()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    write(out)
    if "--sheet" in sys.argv:
        sheet(sys.argv[sys.argv.index("--sheet") + 1])

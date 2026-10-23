"""1-bit 8x8 patterns for Blockworld (1 = black, as on the Mac screen).

A pattern id is 10 bits: id = type * 32 + variant * 16 + face * 4 + fog for block types 1..15,
where variant is the block's checkerboard parity ((x ^ z) & 1; the variant-1 texture is the same
motif shifted, so neighbouring blocks show seams), face is 0 top, 1 x-side, 2 z-side, 3 bottom,
and fog is 0 (near) or 1 (far; 2 and 3 are spare). Type 0 (air) holds the special ids below.
Patterns are anchored to the screen: pixel (x, y) of a piece with id p is bit 7 - (x & 7) of
PAT[p][y & 7].

Look (manager's direction, 2026-10-17): strong face contrast. Tops are white with a sparse
material motif, x-sides about 50 %, z-sides about 75 %, bottoms darker still; each material keeps
a motif (grass tufts, wood grain, stone speckle, clumpy dark leaves, water ripples, fine sand
dots). Sky plain white with a light band at the horizon. Far blocks (fog 1) are calm plain
lighter tones without motifs.
"""
import world as W

NIDS = 1024
# special ids (type 0)
SKY, HAZE, FOGBG, SUN = 0, 1, 2, 3
FARHILL = 12         # 12, 13: distant hills, nearer and farther
FARLINE = 14         # their silhouette edge
LINE = 4             # 4..7: block edge lines, by fog level
TURFBAND = 8         # 8..11: the grass band at the top of a turf block's side, by fog level
TURFBAND_B = 24      # 24..27: same, variant 1

FACE_TOP, FACE_X, FACE_Z, FACE_BOT = 0, 1, 2, 3

BAYER8 = [
    [0, 32, 8, 40, 2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
]


def bayer(level):
    """Ordered-dither pattern with `level` of 64 pixels black."""
    return [sum(((BAYER8[y][x] < level) << (7 - x)) for x in range(8)) for y in range(8)]


def P(*rows):
    assert len(rows) == 8 and all(len(r) == 8 for r in rows), rows
    return [int(r.replace(".", "0").replace("#", "1"), 2) for r in rows]


def shift(p, dx, dy):
    """Pattern p moved right by dx and down by dy (wrapping)."""
    out = [0] * 8
    for y in range(8):
        r = p[(y - dy) & 7]
        out[y] = ((r >> dx) | (r << (8 - dx))) & 0xFF
    return out


def OR(a, b):
    return [a[y] | b[y] for y in range(8)]


def ANDNOT(a, b):
    return [a[y] & ~b[y] & 0xFF for y in range(8)]


def density(p):
    return sum(bin(r).count("1") for r in p) / 64


# --- near patterns: (top, x-side); z-side = x-side | DARK_Z, bottom = x-side | DARK_B ---------
TUFTS = P("........", "..#.....", ".#.#....", "........", "........", ".....#..", "....#.#.", "........")
DIRT_TOP = P("........", ".#...#..", "........", "...#....", "#.......", "......#.", "..#.....", "........")
DIRT = P("#.#..#.#", ".##.#.#.", "#..#.##.", ".#.##..#", "#.#..#.#", ".#.#.##.", "##.#..#.", ".#..#.##")
STONE_TOP = P("........", "..#.....", "........", "......#.", "........", "...#....", "........", "#.......")
STONE = OR(bayer(30), P("........", "..##....", "........", "......#.", "#.......", "....##..", "........", ".#......"))
COBBLE_TOP = P(".##..##.", "#......#", "#......#", ".##..##.", "........", "..#..#..", "..#..#..", "........")
COBBLE = P(".##..##.", "#.##...#", "#...##.#", ".##..##.", "##.##.##", "..#..#..", "#.#..#.#", "##.##.##")
RINGS = P("..####..", ".#....#.", "#..##..#", "#.#..#.#", "#.#..#.#", "#..##..#", ".#....#.", "..####..")
GRAIN = P("#.#..#.#", "#.#.##.#", "#.#..#.#", "#..#.#.#", "#.#..#.#", "#.#.#..#", "#.#..#.#", "#..#.#.#")
PLANK_TOP = P("########", "........", "........", "...#....", "########", "........", "........", "#.......")
PLANKS = P("########", "#.#.#.#.", ".#.#.#.#", "#.#.#.#.", "########", ".#.#.#.#", "#.#.#.#.", ".#...#.#")
LEAF_TOP = P("#..#.#..", ".#....#.", "...##...", "#.#..#.#", ".#...#..", "...#...#", "#.#..#..", ".....#.#")
LEAVES = P("##.#..#.", "#..##.#.", ".##...##", "#..###..", ".#.#..#.", "...##.##", "##...#..", ".##.#..#")
SAND_TOP = P("........", "...#....", "........", ".......#", "........", ".#......", "........", ".....#..")
SAND = bayer(20)
GLASS_TOP = P("#######.", "#.......", "#....#..", "#...#...", "#.......", "#.......", "#.......", "........")
GLASS = P("#######.", "#......#", "#....#.#", "#...#..#", "#......#", "#......#", "#......#", "########")
COAL_TOP = OR(STONE_TOP, P("........", "....##..", "....##..", "........", ".##.....", ".##.....", "........", "........"))
COAL = OR(STONE, P("........", ".##.....", "####....", ".##.....", "....##..", "...####.", "....##..", "........"))
IRON_TOP = OR(STONE_TOP, P("........", "....#.#.", ".....#..", "....#.#.", ".#......", "#.#.....", ".#......", "........"))
IRON = ANDNOT(STONE, P("........", ".###....", ".#.#....", ".###....", "....###.", "....#.#.", "....###.", "........"))
GOLD_TOP = OR(STONE_TOP, P("........", "...#....", "..###...", "...#....", "......#.", ".....###", "......#.", "........"))
GOLD = OR(STONE, P("...#....", "..###...", ".#####..", "..###...", "...#..#.", "......##", ".....###", "......##"))
GEM_TOP = OR(STONE_TOP, P("........", "..#.....", ".#.#....", "..#.....", "........", "....#...", "...#.#..", "....#..."))
GEM = ANDNOT(OR(STONE, P("..#.....", ".#.#....", "#...#...", ".#.#....", "..#..#..", "....#.#.", "...#...#", "....#.#.")),
             P("........", "..#.....", ".#.#....", "..#.....", "........", ".....#..", "....#.#.", ".....#.."))
BEDROCK = P("##.###.#", "#.#.####", "####.#.#", ".#.###.#", "###.#.##", "#.###.#.", ".####.##", "#.#.####")
RIPPLE = P("........", ".###....", "........", "........", "........", ".....###", "........", "........")
WATER = OR(bayer(24), RIPPLE)
BENCH_TOP = P("########", "#..#...#", "#..#...#", "########", "#...#..#", "#...#..#", "########", "........")
BENCH = P("########", "#.#..#.#", "........", "#.#..#.#", "#.#..#.#", "........", "#.#..#.#", "########")

CHEST_TOP = P("########", "#......#", "#......#", "#......#", "########", "#......#", "#......#", "########")
CHEST = P("########", "#.#.#.##", "##.#.#.#", "#..##..#", "#..##..#", "##.#.#.#", "#.#.#.##", "########")
FURN_TOP = OR(STONE_TOP, P("........", ".######.", ".#....#.", ".#....#.", ".#....#.", ".#....#.", ".######.", "........"))
FURNACE = OR(bayer(26), P("########", "#......#", "#.####.#", "#.####.#", "#.####.#", "#......#", "#......#", "########"))
BED_TOP = P("########", "#.....##", "#.....##", "########", "#.#.#.#.", ".#.#.#.#", "#.#.#.#.", ".#.#.#.#")
BED = P("........", "........", "########", "#.#.#.#.", ".#.#.#.#", "########", "#......#", "#......#")
GRAVEL_TOP = OR(bayer(10), P("........", "..##....", "........", ".....##.", "........", ".##.....", "........", "....#..."))
GRAVEL = OR(bayer(22), P("##......", "........", "...##...", "....#...", "......##", "#.......", "..#.....", "........"))
LAVA_TOP = P("........", "..###...", ".#...#..", "........", "......#.", ".....#.#", "........", "........")
LAVA = P("........", "..##....", "........", "......#.", "........", ".#......", "....##..", "........")
BRICK_TOP = P("########", "...#....", "...#....", "########", ".......#", ".......#", "########", "...#....")
BRICKS = OR(bayer(12), P("########", "...#...#", "...#...#", "########", ".#...#..", ".#...#..", "########", "...#...#"))
CLAY_TOP = bayer(8)
CLAY = OR(bayer(16), P("........", "........", "..#.....", "........", "........", "......#.", "........", "........"))
DOOR_TOP = PLANK_TOP
DOOR = P("#..#..#.", "#..#..#.", "#..#..#.", "#..#.##.", "#..#.##.", "#..#..#.", "#..#..#.", "########")
LADDER_TOP = P("#......#", "#......#", "########", "#......#", "#......#", "#......#", "########", "#......#")
LADDER = P("#......#", "#......#", "########", "#......#", "#......#", "#......#", "########", "#......#")
FENCE_TOP = P("........", "........", "..##....", "..##....", "........", "........", "......##", "......##")
FENCE = P("..##..##", "..##..##", "########", "..##..##", "..##..##", "########", "..##..##", "..##..##")
FARM_TOP = P("########", "........", "..#..#..", "........", "########", "........", ".#...#..", "........")
WOOL_TOP = P("..##....", ".#..#...", ".#..#..#", "..##..#.", "....##..", "...#..#.", "#..#..#.", ".#..##..")
WOOL = OR(bayer(8), WOOL_TOP)

NEAR = {
    W.TURF: (TUFTS, DIRT),
    W.SOIL: (DIRT_TOP, DIRT),
    W.ROCK: (STONE_TOP, STONE),
    W.RUBBLE: (COBBLE_TOP, COBBLE),
    W.LOG: (RINGS, GRAIN),
    W.BOARDS: (PLANK_TOP, PLANKS),
    W.LEAVES: (LEAF_TOP, LEAVES),
    W.SAND: (SAND_TOP, SAND),
    W.GLASS: (GLASS_TOP, GLASS),
    W.COAL: (COAL_TOP, COAL),
    W.IRON: (IRON_TOP, IRON),
    W.BEDROCK: (BEDROCK, BEDROCK),
    W.WATER: (RIPPLE, WATER),
    W.BENCH: (BENCH_TOP, BENCH),
    W.GOLD: (GOLD_TOP, GOLD),
    W.GEM: (GEM_TOP, GEM),
    W.CHEST: (CHEST_TOP, CHEST),
    W.FURNACE: (FURN_TOP, FURNACE),
    W.BED: (BED_TOP, BED),
    W.GRAVEL: (GRAVEL_TOP, GRAVEL),
    W.LAVA: (LAVA_TOP, LAVA),
    W.BRICKS: (BRICK_TOP, BRICKS),
    W.CLAY: (CLAY_TOP, CLAY),
    W.DOOR: (DOOR_TOP, DOOR),
    W.LADDER: (LADDER_TOP, LADDER),
    W.FENCE: (FENCE_TOP, FENCE),
    W.FARMLAND: (FARM_TOP, DIRT),
    W.WOOL: (WOOL_TOP, WOOL),
}
DARK_Z = bayer(44)          # z-sides: x-side texture | 44/64 -> about 75 %
DARK_B = bayer(52)          # bottoms
# far (fog 1): plain tones (Bayer levels) for top, x, z, bottom
FAR = {
    W.TURF: (0, 14, 24, 30), W.SOIL: (2, 14, 24, 30), W.ROCK: (0, 12, 22, 30),
    W.RUBBLE: (2, 14, 24, 30), W.LOG: (4, 18, 28, 34), W.BOARDS: (2, 12, 22, 30),
    W.LEAVES: (12, 20, 30, 36), W.SAND: (0, 6, 16, 24), W.GLASS: (0, 4, 10, 16),
    W.COAL: (2, 14, 24, 30), W.IRON: (2, 14, 24, 30), W.BEDROCK: (32, 36, 42, 46),
    W.WATER: (2, 12, 22, 28), W.BENCH: (4, 14, 24, 30), W.GOLD: (2, 14, 24, 30), W.GEM: (2, 14, 24, 30),
    W.CHEST: (4, 16, 26, 32), W.FURNACE: (6, 18, 28, 34), W.BED: (4, 12, 22, 30), W.GRAVEL: (6, 18, 28, 34),
    W.LAVA: (0, 2, 8, 14), W.BRICKS: (6, 18, 28, 34), W.CLAY: (4, 12, 22, 30), W.DOOR: (4, 16, 26, 32),
    W.LADDER: (4, 14, 24, 30), W.FENCE: (4, 14, 24, 30), W.FARMLAND: (6, 14, 24, 30), W.WOOL: (2, 8, 18, 26),
}
FAR_WATER_TOP = P("........", "........", "........", "...###..", "........", "........", "........", "........")

TURF_BAND = P("#.#.#.#.", ".###.###", "##.###.#", ".#.#.#.#", "#.#.#.#.", ".###.###", "##.###.#", ".#.#.#.#")


DIM, DARK = bayer(22), bayer(46)   # the light's dither over near textures (shade 2, 3)


def face_pattern(t, face, v, far):
    """8 row bytes for block type t, face (0..3), variant v, shade far: 0 near, 1 far (fog),
    2 dim (night, under trees), 3 dark (caves)."""
    if far >= 2:
        return OR(face_pattern(t, face, v, 0), DIM if far == 2 else DARK)
    if far:
        if t == W.WATER and face == 0:
            return FAR_WATER_TOP
        return bayer(FAR[t][face])
    top, side = NEAR[t]
    if face == 0:
        p = top
    elif face == 1:
        p = side
    elif face == 2:
        p = OR(side, bayer(28) if t == W.LEAVES else DARK_Z)
    else:
        p = OR(side, bayer(36) if t == W.LEAVES else DARK_B)
    if v:
        p = shift(p, 4, 4) if face else shift(p, 3, 5)
    return p


def build():
    """PAT[NIDS][8]: 8 row bytes per id."""
    pat = [[0] * 8 for _ in range(NIDS)]
    pat[SKY] = [0] * 8
    pat[HAZE] = bayer(1)
    pat[FOGBG] = bayer(2)
    pat[FARHILL] = bayer(6)
    pat[FARHILL + 1] = bayer(4)
    pat[FARLINE] = [0x88 if y & 1 else 0x22 for y in range(8)]
    pat[SUN] = [0] * 8
    for f in range(4):
        far = f == 1
        pat[LINE + f] = [0x55 if y & 1 else 0xAA for y in range(8)] if far else [0xFF] * 8
        tb = bayer(28) if far else TURF_BAND if f == 0 else OR(TURF_BAND, DIM if f == 2 else DARK)
        pat[TURFBAND + f] = tb
        pat[TURFBAND_B + f] = tb if far else shift(tb, 4, 0)
    for t in NEAR:
        for v in range(2):
            for face in range(4):
                for f in range(4):
                    pat[t * 32 + v * 16 + face * 4 + f] = face_pattern(t, face, v, f)
    return pat


PAT = build()

if __name__ == "__main__":
    for t in NEAR:
        print(f"{W.NAMES[t]:8s}", " ".join(f"{density(face_pattern(t, f, 0, 0)):.2f}" for f in range(4)))

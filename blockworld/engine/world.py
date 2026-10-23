"""Blockworld world: block types, seeded terrain generator, column segment lists.

Everything is integer arithmetic on uint32 values with explicit masks, written so the C port
(app/world.c) produces the identical world from the same seed.

World: WX x WX columns (x, z), WY blocks high (y): 128 x 128 x 48, 1 byte per block,
blocks[(x * WX + z) * WY + y]. Outside the square is open sea for rendering (the island's
ocean); the player can't leave the square.
"""
import math as _m

WX = 128         # world size in x and z
WY = 48          # world height
SEA = 27         # water fills y <= SEA where the terrain is lower

# Block types (original names; 0 = air). Types 17..31 are reserved for later blocks.
(AIR, TURF, SOIL, ROCK, RUBBLE, LOG, BOARDS, LEAVES, SAND, GLASS, COAL, IRON, BEDROCK, WATER,
 BENCH, GOLD, GEM, CHEST, FURNACE, BED, GRAVEL, LAVA, BRICKS, CLAY, DOOR, LADDER, FENCE, FARMLAND,
 WOOL) = range(29)
# (torches, plants and open doors are objects of the game, not blocks: the block there is air)
NTYPES = 32
NAMES = ["air", "turf", "soil", "rock", "rubble", "log", "boards", "leaves", "sand", "glass",
         "coal", "iron", "bedrock", "water", "bench", "gold", "gem", "chest", "furnace", "bed",
         "gravel", "lava", "bricks", "clay", "door", "ladder", "fence", "farmland", "wool"] + ["?"] * 3
SOLID = [t not in (AIR, WATER, LAVA, LADDER) for t in range(NTYPES)]

M32 = 0xFFFFFFFF


def hash3(a, b, c):
    """32-bit integer hash of three ints (the C port uses uint32 wrap-around)."""
    h = (a * 0x9E3779B1 + b * 0x85EBCA6B + c * 0xC2B2AE35) & M32
    h ^= h >> 15
    h = (h * 0x2C1B3C6D) & M32
    h ^= h >> 12
    h = (h * 0x297A2D39) & M32
    h ^= h >> 15
    return h


class Rng:
    """xorshift32."""
    def __init__(self, seed):
        self.s = (seed & M32) or 0x12345678

    def next(self):
        s = self.s
        s ^= (s << 13) & M32
        s ^= s >> 17
        s ^= (s << 5) & M32
        self.s = s
        return s

    def below(self, n):          # 0 .. n-1 (n < 65536)
        return ((self.next() >> 16) * n) >> 16


def smooth(t):
    """smoothstep on 0..255 -> 0..255 (3t^2 - 2t^3)."""
    return (t * t * (768 - 2 * t)) >> 16


def vnoise(x, z, shift, seed):
    """Value noise, lattice spacing 1 << shift, values 0..255, smooth bilinear."""
    gx, gz = x >> shift, z >> shift
    m = (1 << shift) - 1
    tx = smooth(((x & m) << 8) >> shift)
    tz = smooth(((z & m) << 8) >> shift)
    a = hash3(gx, gz, seed) & 255
    b = hash3(gx + 1, gz, seed) & 255
    c = hash3(gx, gz + 1, seed) & 255
    d = hash3(gx + 1, gz + 1, seed) & 255
    ab = a + (((b - a) * tx) >> 8)
    cd = c + (((d - c) * tx) >> 8)
    return ab + (((cd - ab) * tz) >> 8)


def idx(x, z, y):
    return (x * WX + z) * WY + y


def height_map(seed):
    """Top solid block y for each column (before caves and trees)."""
    hm = [0] * (WX * WX)
    for x in range(WX):
        for z in range(WX):
            n1 = vnoise(x + 7, z + 3, 4, seed + 101)       # 16-block hills
            n2 = vnoise(x, z, 3, seed + 202)               # 8-block bumps
            n3 = vnoise(x, z, 2, seed + 303)               # 4-block detail
            rough = vnoise(x + 1, z + 5, 5, seed + 404)    # 32-block: how hilly this area is
            big = vnoise(x + 3, z + 9, 6, seed + 808)      # 64-block: lowlands and highlands
            hill = ((n1 - 128) * (6 + (rough >> 4))) >> 7  # -11 .. +10
            h = 31 + hill + ((n2 - 128) * 3 >> 7) + ((n3 - 128) >> 7) + ((big - 128) * 6 >> 7)
            # island: lower the land towards the edges of the square
            dx = 2 * x - (WX - 1)
            dz = 2 * z - (WX - 1)
            r2 = dx * dx + dz * dz
            if r2 > 11600:
                h -= (r2 - 11600) >> 8
            if h < 5:
                h = 5
            if h > WY - 8:
                h = WY - 8
            hm[x * WX + z] = h
    return hm


SIN256 = [int(round(256 * _m.sin(2 * _m.pi * i / 256))) for i in range(256)]
COS256 = [SIN256[(i + 64) & 255] for i in range(256)]

ORES = ((COAL, 120, 40, 7), (IRON, 64, 30, 5), (GOLD, 20, 16, 4), (GEM, 10, 10, 3))   # type, veins, max y, size


def generate(seed):
    """Return (blocks bytearray, heightmap list). Deterministic for a seed."""
    seed &= 0xFFFF
    b = bytearray(WX * WX * WY)
    hm = height_map(seed)
    # terrain layers
    for x in range(WX):
        for z in range(WX):
            h = hm[x * WX + z]
            base = idx(x, z, 0)
            b[base] = BEDROCK
            if hash3(x, z, seed + 505) & 1:
                b[base + 1] = BEDROCK
            beach = h <= SEA + 1
            for y in range(1, h + 1):
                if b[base + y]:
                    continue
                if y == h:
                    b[base + y] = SAND if beach else TURF
                elif y >= h - 3:
                    b[base + y] = SAND if beach else SOIL
                else:
                    b[base + y] = ROCK
            for y in range(h + 1, SEA + 1):
                b[base + y] = WATER
    rng = Rng(hash3(seed, 77, 99))

    def at(x, z, y):
        return b[idx(x, z, y)]

    # caves: random-walk worms carving small balls under the surface
    for _ in range(28):
        x = (8 + rng.below(WX - 16)) << 8
        z = (8 + rng.below(WX - 16)) << 8
        y = (3 + rng.below(26)) << 8
        dirn = rng.below(256)
        dy = 0
        for _step in range(60 + rng.below(60)):
            dirn = (dirn + rng.below(41) - 20) & 255
            dy = dy + rng.below(17) - 8
            if dy > 24:
                dy = 24
            if dy < -24:
                dy = -24
            x += COS256[dirn] >> 2
            z += SIN256[dirn] >> 2
            y += dy
            if y < (3 << 8):
                y = 3 << 8
                dy = 0
            if y > (WY - 8) << 8:
                y = (WY - 8) << 8
                dy = 0
            r = 1 + (rng.below(4) == 0)
            cx, cz, cy = x >> 8, z >> 8, y >> 8
            for xx in range(cx - r, cx + r + 1):
                for zz in range(cz - r, cz + r + 1):
                    for yy in range(cy - r, cy + r + 1):
                        if not (1 <= xx < WX - 1 and 1 <= zz < WX - 1 and 2 <= yy < WY - 1):
                            continue
                        if (xx - cx) ** 2 + (zz - cz) ** 2 + (yy - cy) ** 2 > r * r + 1:
                            continue
                        t = at(xx, zz, yy)
                        if t in (AIR, WATER, BEDROCK):
                            continue
                        # don't open into water: skip if water is next to this block
                        wet = (at(xx, zz, yy + 1) == WATER or at(xx + 1, zz, yy) == WATER or
                               at(xx - 1, zz, yy) == WATER or at(xx, zz + 1, yy) == WATER or
                               at(xx, zz - 1, yy) == WATER)
                        if not wet:
                            b[idx(xx, zz, yy)] = AIR

    # ore veins in rock, by depth
    for ore, count, ymax, size in ORES:
        for _ in range(count):
            for _try in range(8):              # start inside rock
                x = 1 + rng.below(WX - 2)
                z = 1 + rng.below(WX - 2)
                y = 2 + rng.below(ymax - 2)
                if at(x, z, y) == ROCK:
                    break
            for _k in range(size):
                if at(x, z, y) == ROCK:
                    b[idx(x, z, y)] = ore
                m = rng.below(6)
                if m == 0 and x < WX - 2:
                    x += 1
                elif m == 1 and x > 1:
                    x -= 1
                elif m == 2 and z < WX - 2:
                    z += 1
                elif m == 3 and z > 1:
                    z -= 1
                elif m == 4 and y < ymax:
                    y += 1
                elif y > 2:
                    y -= 1

    # world depth (its own random stream): ravines, lava in the deepest layers, gravel, clay
    rng = Rng(hash3(seed, 88, 11))

    def dry(xx, zz, yy):                       # no water above or beside
        return not (at(xx, zz, yy + 1) == WATER or at(xx + 1, zz, yy) == WATER or at(xx - 1, zz, yy) == WATER
                    or at(xx, zz + 1, yy) == WATER or at(xx, zz - 1, yy) == WATER)

    for _ in range(4):                         # ravines: deep narrow cuts, half a block a step
        x = (16 + rng.below(WX - 32)) << 8
        z = (16 + rng.below(WX - 32)) << 8
        dirn = rng.below(256)
        length = 30 + rng.below(30)
        floor = 3 + rng.below(5)
        for step in range(length):
            dirn = (dirn + rng.below(9) - 4) & 255
            x += COS256[dirn] >> 1
            z += SIN256[dirn] >> 1
            cx, cz = x >> 8, z >> 8
            r = 0 if step < 5 or step >= length - 5 else 1
            for xx in range(cx - r, cx + r + 1):
                for zz in range(cz - r, cz + r + 1):
                    if abs(xx - cx) + abs(zz - cz) > r or not (1 <= xx < WX - 1 and 1 <= zz < WX - 1):
                        continue
                    h = hm[xx * WX + zz]
                    if h <= SEA + 1:
                        continue
                    for yy in range(floor + (step & 1), h + 1):
                        t = at(xx, zz, yy)
                        if t not in (AIR, WATER, BEDROCK) and dry(xx, zz, yy):
                            b[idx(xx, zz, yy)] = AIR
    for x in range(1, WX - 1):                 # lava: air at y 2..4 (deep cave and ravine floors)
        for z in range(1, WX - 1):
            for y in range(2, 5):
                if at(x, z, y) == AIR:
                    b[idx(x, z, y)] = LAVA
    for _ in range(40):                        # gravel pockets in rock
        x, z, y = 1 + rng.below(WX - 2), 1 + rng.below(WX - 2), 3 + rng.below(36)
        for _k in range(10):
            if at(x, z, y) == ROCK:
                b[idx(x, z, y)] = GRAVEL
            m = rng.below(6)
            if m == 0 and x < WX - 2:
                x += 1
            elif m == 1 and x > 1:
                x -= 1
            elif m == 2 and z < WX - 2:
                z += 1
            elif m == 3 and z > 1:
                z -= 1
            elif m == 4 and y < 40:
                y += 1
            elif y > 3:
                y -= 1
    for _ in range(16):                        # clay patches on the sea floor
        x, z = 2 + rng.below(WX - 4), 2 + rng.below(WX - 4)
        for xx in range(x - 2, x + 3):
            for zz in range(z - 2, z + 3):
                h = hm[xx * WX + zz]
                if (xx - x) ** 2 + (zz - z) ** 2 <= 5 and h < SEA and at(xx, zz, h) == SAND:
                    b[idx(xx, zz, h)] = CLAY

    # trees on turf, spaced out
    trees = []
    for x in range(3, WX - 3):
        for z in range(3, WX - 3):
            h = hm[x * WX + z]
            if at(x, z, h) != TURF or h > WY - 10:
                continue
            if (hash3(x, z, seed + 606) & 127) >= 3:
                continue
            if any(abs(tx - x) < 6 and abs(tz - z) < 6 for tx, tz in trees):
                continue
            trees.append((x, z))
            th = 4 + (hash3(x, z, seed + 707) & 1)
            top = h + th                      # y of the highest log
            for yy in range(top - 2, top + 2):
                rad = 2 if yy < top else 1
                for xx in range(x - rad, x + rad + 1):
                    for zz in range(z - rad, z + rad + 1):
                        corner = abs(xx - x) == rad and abs(zz - z) == rad
                        if corner and (rad == 1 or (hash3(xx, zz, seed + yy) & 1)):
                            continue
                        if at(xx, zz, yy) == AIR:
                            b[idx(xx, zz, yy)] = LEAVES
            for yy in range(h + 1, top + 1):
                b[idx(x, z, yy)] = LOG
            b[idx(x, z, h)] = SOIL
    return b, hm


TOPEXP, BOTEXP = 1, 2      # segment flags: air above the top / air below the bottom


def column_segments(b, x, z):
    """Vertical runs of one block type, top first: [(top, bot, type, flags, light class)] with top =
    y of the top surface (highest block + 1), bot = y of the lowest block, flags TOPEXP if air is
    above it (or it reaches the top of the world) and BOTEXP if air is below it (not at y = 0)."""
    segs = []
    base = idx(x, z, 0)
    y = WY - 1
    while y >= 0:
        t = b[base + y]
        if t == AIR:
            y -= 1
            continue
        top = y + 1
        while y >= 0 and b[base + y] == t:
            y -= 1
        bot = y + 1
        f = (TOPEXP if top == WY or b[base + top] == AIR else 0) | (BOTEXP if bot > 0 and b[base + bot - 1] == AIR else 0)
        cls = 0 if not segs else (1 if segs[-1][2] in (LEAVES, LOG) else 2)
        segs.append((top, bot, t, f, cls))
    return segs


# light classes of segments: 0 open to the sky (the column's first), 1 under a tree (leaves or a
# log right above), 2 covered (caves, overhangs), 3 lit by a torch (set by the game)
OCEAN = [(SEA + 1, 0, WATER, TOPEXP, 0)]


def all_segments(b):
    """segs[x * WX + z] for the whole world."""
    return [column_segments(b, x, z) for x in range(WX) for z in range(WX)]


CW = WX // 4         # coarse map: 4 x 4 block regions


def coarse_heights(b):
    """Highest surface (top of the highest non-air block) per 4 x 4 block region."""
    ch = [0] * (CW * CW)
    for x in range(WX):
        for z in range(WX):
            base = idx(x, z, 0)
            y = WY - 1
            while y > 0 and b[base + y] == AIR:
                y -= 1
            i = (x >> 2) * CW + (z >> 2)
            if y + 1 > ch[i]:
                ch[i] = y + 1
    return ch


def make_farh(ch):
    """farh(region x, region z): coarse height, the sea outside the world."""
    def farh(rx, rz):
        if 0 <= rx < CW and 0 <= rz < CW:
            return ch[rx * CW + rz]
        return SEA + 1
    return farh

"""Tank Duel: the single source of the simulation's constants, tables, tank types and arenas.

Both versions of the simulation read these: the C code through sim_tables.c/.h, which
gen_tables.py writes from this file, and the numpy twin (twin.py) by importing it. Every table
is computed here once, so the two can't disagree.

Units: 1 pixel = 16 sub-units (SU); a tile is 16 px = 256 SU; angles are 0..255 (0 = east,
64 = south, the screen's y grows downwards); the simulation runs 30 ticks per second.
"""
import math

TW, TH = 24, 18          # arena size in tiles (384 x 288 px)
SU = 16                  # sub-units per pixel
TS = 256                 # tile size in SU
MAXT = 4                 # tanks
MAXS = 16                # shells
HB = 112                 # tank half box for walls and other tanks (7 px)
HIT = 96                 # shell-to-tank hit half box (6 px)
SHELL_HIT = 64           # shell-to-shell half box (4 px)
SHELL_LIFE = 240         # ticks (8 s)
BARREL = 160             # barrel tip from the tank's centre (10 px)
RAYSTEP = 128            # sensor ray step (8 px)
RAYN = 20                # sensor ray steps (160 px)
NRAY = 8
AIMSTEP = 128            # predicted shell path step (8 px)
AIMN = 40                # predicted shell path steps (320 px)
AIMHIT = 128             # predicted path "hits" a tank within 8 px
NOBS = 40
DECIDE = 3               # an AI tank decides every 3 ticks (10 Hz), at tick % 3 == index % 3

# sin table, Q14: SIN[a] = round(16384 sin(2 pi a / 256)); COS(a) = SIN[(a + 64) & 255]
SIN = [int(round(16384 * math.sin(2 * math.pi * a / 256))) for a in range(256)]
# atan table: ATAN[k] = round(atan(k / 64) in angle units), k = 0..64 -> 0..32
ATAN = [int(round(math.atan(k / 64) * 256 / (2 * math.pi))) for k in range(65)]

# Policies sample their actions (temperature 0.5): action = argmax(z + noise) per head, where z are
# the integer logits (scale 2^SH of the output layer) and noise = GUMBEL_Q8[byte] << (SH - 9), the
# bytes coming from the simulation's random generator (sim_pick). GUMBEL_Q8 is 256 x a Gumbel sample
# plus 512, so that it is never negative (C can shift it left safely; a common offset changes no argmax).
GUMBEL_Q8 = [int(round(256 * -math.log(-math.log((k + 0.5) / 256)))) + 512 for k in range(256)]
PICK_SHIFT = 9            # noise = G << (SH - PICK_SHIFT): 9 is temperature 0.5
HEADS = (3, 3, 7, 2)

# Tank types: speeds in SU per tick, turning in angle units per tick, cooldown in ticks
TYPE_FIELDS = ("maxf", "maxr", "acc", "turn", "trate", "sspeed", "cool", "nmax")
TYPES = [
    # name       maxf maxr acc turn trate sspeed cool nmax
    ("player",    20,  14,  3,   4,   8,    56,   15,  3),
    ("rusher",    24,  14,  4,   5,   8,    56,   12,  3),
    ("sniper",    14,  10,  2,   3,   6,    80,   30,  2),
    ("careful",   18,  16,  3,   4,   7,    60,   20,  2),
]
T_PLAYER, T_RUSHER, T_SNIPER, T_CAREFUL = 0, 1, 2, 3

# Events the simulation reports for sound and effects (kind, x, y, a)
EV_FIRE, EV_RICOCHET, EV_SHELL_END, EV_KILL, EV_CLANG, EV_SHELLS, EV_DUD, EV_SHIELD = range(1, 9)
MAXEV = 24

# Scripted bots
BOT_SITTER, BOT_HUNTER, BOT_WANDER = 1, 2, 3

# Arenas: '#' wall, '.' floor, 'P' player spawn, 'a' 'b' 'c' enemy spawns (on floor).
ARENAS = [
    ("Crossing", [
        "########################",
        "#......................#",
        "#.P....................#",
        "#......##......##......#",
        "#......##......##......#",
        "#......................#",
        "#..........##..........#",
        "#...###....##....###...#",
        "#......................#",
        "#......................#",
        "#...###....##....###...#",
        "#..........##..........#",
        "#......................#",
        "#......##......##......#",
        "#......##......##....a.#",
        "#.....................b#",
        "#....................c.#",
        "########################"]),
    ("Corridors", [
        "########################",
        "#..........#...........#",
        "#.P........#........a..#",
        "#..........#...........#",
        "#...####.......####....#",
        "#......................#",
        "#..........##..........#",
        "####.......##.......####",
        "#......#........#......#",
        "#......#........#......#",
        "####.......##.......####",
        "#..........##..........#",
        "#......................#",
        "#...####.......####....#",
        "#..........#...........#",
        "#..b.......#........c..#",
        "#..........#...........#",
        "########################"]),
    ("Pillars", [
        "########################",
        "#......................#",
        "#.P....................#",
        "#....#....#....#....#..#",
        "#......................#",
        "#......................#",
        "#..#....#....#....#....#",
        "#......................#",
        "#......................#",
        "#....#....#....#....#..#",
        "#......................#",
        "#......................#",
        "#..#....#....#....#....#",
        "#......................#",
        "#..................a...#",
        "#....#....#....#....#..#",
        "#..b................c..#",
        "########################"]),
    ("Fortress", [
        "########################",
        "#......................#",
        "#.P....................#",
        "#......................#",
        "#.......###..###.......#",
        "#.......#........#.....#",
        "#.......#..............#",
        "#..........a...........#",
        "#....###........###....#",
        "#......................#",
        "#.......#...b..........#",
        "#.......#........#.....#",
        "#.......###..###.......#",
        "#......................#",
        "#......................#",
        "#..................c...#",
        "#......................#",
        "########################"]),
    ("Zigzag", [
        "########################",
        "#......................#",
        "#.P....#...............#",
        "#......#.......#.......#",
        "#......#.......#.......#",
        "#......#.......#...a...#",
        "#..............#.......#",
        "#..............#.......#",
        "#.....####.....#####...#",
        "#..........#...........#",
        "#..........#...........#",
        "#....#.....#...........#",
        "#....#.....#.......#...#",
        "#....#.............#...#",
        "#....#.......b.....#.c.#",
        "#....#.............#...#",
        "#......................#",
        "########################"]),
    ("Courtyard", [
        "########################",
        "#......................#",
        "#.P....................#",
        "#...##############.....#",
        "#...#..................#",
        "#...#..................#",
        "#...#.....#....#...#...#",
        "#...#..............#...#",
        "#...#....a.........#...#",
        "#...#..............#...#",
        "#...#.....#....#...#...#",
        "#..................#...#",
        "#..................#...#",
        "#.....##############...#",
        "#.....................b#",
        "#.c....................#",
        "#......................#",
        "########################"]),
    ("Bunkers", [
        "########################",
        "#......................#",
        "#.P..#.........#.......#",
        "#....#.........#.......#",
        "#.####...###...####....#",
        "#......................#",
        "#.........#.#..........#",
        "#...##....#.#.....##...#",
        "#...#..............#...#",
        "#...#..............#...#",
        "#...##....#.#.....##...#",
        "#.........#.#..........#",
        "#......................#",
        "#....####...###...####.#",
        "#.......#.........#....#",
        "#..b....#.....a...#..c.#",
        "#......................#",
        "########################"]),
    ("Maze", [
        "########################",
        "#......#...............#",
        "#.P....#.....#.........#",
        "#......#.....#.....#...#",
        "#..#####.....#.....#...#",
        "#............#.....#...#",
        "#........#...#######...#",
        "#........#.............#",
        "####.....#.............#",
        "#........#....####.....#",
        "#.....####.......#.....#",
        "#................#..a..#",
        "#.....#..........#.....#",
        "#.....#....#######.....#",
        "#.....#................#",
        "#..b..#...........c....#",
        "#.....#................#",
        "########################"]),
]


def arena(i):
    """(name, wall grid [TH][TW] of 0/1, spawns {'P': (tx, ty), 'a': ...})."""
    name, rows = ARENAS[i]
    assert len(rows) == TH, (name, len(rows))
    grid, spawns = [], {}
    for ty, row in enumerate(rows):
        assert len(row) == TW, (name, ty, len(row))
        g = []
        for tx, ch in enumerate(row):
            g.append(1 if ch == "#" else 0)
            if ch in "Pabc":
                spawns[ch] = (tx, ty)
        grid.append(g)
    for tx in range(TW):
        assert grid[0][tx] and grid[TH - 1][tx], name
    for ty in range(TH):
        assert grid[ty][0] and grid[ty][TW - 1], name
    return name, grid, spawns

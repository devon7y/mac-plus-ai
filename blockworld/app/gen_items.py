#!/usr/bin/env python3
"""Blockworld items: ids, names, stack sizes, durability, mining rules, recipes, smelting, fuel and
the 16x16 1-bit icons (original art). The one source of truth; writes OUTDIR/items.h and items.c.
    gen_items.py OUTDIR [--png FILE]   (--png: a sheet of every icon, for looking at them)

Item ids: 1..28 are the block types of world.h (placing one puts that block); 29 torch and
30 sapling are placed as game objects; 32.. are things (materials, food, the bow and arrows);
64 + tier * 5 + kind are tools (kind: pick, axe, shovel, sword, hoe; tier: wood, rock, iron, gold,
gem); 96 + material * 4 + piece are armour (material: hide, iron, gold, gem; piece: helmet, tunic,
leggings, boots).
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "engine"))
import world as W  # noqa: E402
import patterns as PT  # noqa: E402

# ---- ids ---------------------------------------------------------------------------------------
TORCH, SAPLING = 29, 30
THINGS = ["stick", "coal", "iron bar", "gold bar", "gem", "flint", "clay lump", "brick", "wheat",
          "seeds", "bread", "apple", "raw meat", "cooked meat", "hide", "feather", "string", "arrow",
          "bow"]
T = {n: 32 + i for i, n in enumerate(THINGS)}
KINDS = ["pick", "axe", "shovel", "sword", "hoe"]
TIERS = ["wood", "rock", "iron", "gold", "gem"]
PIECES = ["helmet", "tunic", "leggings", "boots"]
MATS = ["hide", "iron", "gold", "gem"]


def tool(tier, kind):
    return 64 + tier * 5 + kind


def armour(mat, piece):
    return 96 + mat * 4 + piece


NITEMS = 112
names = [""] * NITEMS
stack = [0] * NITEMS
dur = [0] * NITEMS
food = [0] * NITEMS          # hunger points restored
for t in range(1, 29):
    if t not in (W.BEDROCK, W.WATER, W.LAVA, W.FARMLAND):
        names[t] = W.NAMES[t]
        stack[t] = 64
names[TORCH], stack[TORCH] = "torch", 64
names[SAPLING], stack[SAPLING] = "sapling", 64
names[W.COAL], names[W.IRON], names[W.GOLD], names[W.GEM] = "coal ore", "iron ore", "gold ore", "gem ore"
names[W.BENCH] = "workbench"
for n, i in T.items():
    names[i], stack[i] = n, 64
stack[T["bow"]] = 1
for n, f in (("bread", 5), ("apple", 4), ("raw meat", 3), ("cooked meat", 8)):
    food[T[n]] = f
TOOL_DUR = [59, 131, 250, 32, 1561]
for ti in range(5):
    for k in range(5):
        i = tool(ti, k)
        names[i], stack[i], dur[i] = f"{TIERS[ti]} {KINDS[k]}", 1, TOOL_DUR[ti]
dur[T["bow"]] = 384
ARMOUR_DUR = [[55, 80, 75, 65], [165, 240, 225, 195], [77, 112, 105, 91], [363, 528, 495, 429]]
ARMOUR_DEF = [[1, 3, 2, 1], [2, 6, 5, 2], [2, 5, 3, 1], [3, 8, 6, 3]]     # armour points (of 20)
armour_def = [0] * NITEMS
for m in range(4):
    for p in range(4):
        i = armour(m, p)
        names[i], stack[i], dur[i] = f"{MATS[m]} {PIECES[p]}", 1, ARMOUR_DUR[m][p]
        armour_def[i] = ARMOUR_DEF[m][p]

# ---- mining: hardness (1/10 s units of the original rules), preferred tool, harvest level ------
# tool: 0 any, 1 pick, 2 axe, 3 shovel; level: -1 no tool needed, else the pick tier needed
PICK, AXE, SHOVEL = 1, 2, 3
mine = {  # type: (hardness x 10, tool, level, drop, count)
    W.TURF: (6, SHOVEL, -1, W.SOIL, 1), W.SOIL: (5, SHOVEL, -1, W.SOIL, 1), W.ROCK: (15, PICK, 0, W.RUBBLE, 1),
    W.RUBBLE: (20, PICK, 0, W.RUBBLE, 1), W.LOG: (20, AXE, -1, W.LOG, 1), W.BOARDS: (20, AXE, -1, W.BOARDS, 1),
    W.LEAVES: (2, 0, -1, 0, 0), W.SAND: (5, SHOVEL, -1, W.SAND, 1), W.GLASS: (3, 0, -1, 0, 0),
    W.COAL: (30, PICK, 0, T["coal"], 1), W.IRON: (30, PICK, 1, W.IRON, 1), W.BEDROCK: (0, 0, 99, 0, 0),
    W.WATER: (0, 0, 99, 0, 0), W.BENCH: (25, AXE, -1, W.BENCH, 1), W.GOLD: (30, PICK, 2, W.GOLD, 1),
    W.GEM: (30, PICK, 2, T["gem"], 1), W.CHEST: (25, AXE, -1, W.CHEST, 1), W.FURNACE: (35, PICK, 0, W.FURNACE, 1),
    W.BED: (2, 0, -1, W.BED, 1), W.GRAVEL: (6, SHOVEL, -1, W.GRAVEL, 1), W.LAVA: (0, 0, 99, 0, 0),
    W.BRICKS: (20, PICK, 0, W.BRICKS, 1), W.CLAY: (6, SHOVEL, -1, T["clay lump"], 4), W.DOOR: (30, AXE, -1, W.DOOR, 1),
    W.LADDER: (4, AXE, -1, W.LADDER, 1), W.FENCE: (20, AXE, -1, W.FENCE, 1), W.FARMLAND: (6, SHOVEL, -1, W.SOIL, 1),
    W.WOOL: (8, 0, -1, W.WOOL, 1),
}
TOOL_SPEED = [2, 4, 6, 12, 8]     # wood, rock, iron, gold, gem
TIER_LEVEL = [0, 1, 2, 0, 3]      # what each tier can harvest (gold: like wood)

# ---- recipes: rows of keys, key map, output, count --------------------------------------------
P_, S_ = W.BOARDS, T["stick"]
recipes = [
    (["L"], {"L": W.LOG}, W.BOARDS, 4),
    (["P", "P"], {"P": P_}, S_, 4),
    (["PP", "PP"], {"P": P_}, W.BENCH, 1),
    (["C", "S"], {"C": T["coal"], "S": S_}, TORCH, 4),
    (["PPP", "P P", "PPP"], {"P": P_}, W.CHEST, 1),
    (["RRR", "R R", "RRR"], {"R": W.RUBBLE}, W.FURNACE, 1),
    (["WWW", "PPP"], {"W": W.WOOL, "P": P_}, W.BED, 1),
    (["PP", "PP", "PP"], {"P": P_}, W.DOOR, 3),
    (["S S", "SSS", "S S"], {"S": S_}, W.LADDER, 3),
    (["SPS", "SPS"], {"S": S_, "P": P_}, W.FENCE, 3),
    (["WWW"], {"W": T["wheat"]}, T["bread"], 1),
    (["BB", "BB"], {"B": T["brick"]}, W.BRICKS, 1),
    (["CC", "CC"], {"C": T["clay lump"]}, W.CLAY, 1),
    (["SS", "SS"], {"S": T["string"]}, W.WOOL, 1),
    (["RR", "RR"], {"R": W.SAND}, W.GLASS, 0),   # (placeholder count 0: removed below)
    ([" SR", "S R", " SR"], {"S": S_, "R": T["string"]}, T["bow"], 1),
    (["F", "S", "E"], {"F": T["flint"], "S": S_, "E": T["feather"]}, T["arrow"], 4),
]
recipes = [r for r in recipes if r[3]]
MATERIAL = [P_, W.RUBBLE, T["iron bar"], T["gold bar"], T["gem"]]
TOOL_SHAPES = [["MMM", " S ", " S "], ["MM", "MS", " S"], ["M", "S", "S"], ["M", "M", "S"], ["MM", " S", " S"]]
for ti in range(5):
    for k in range(5):
        recipes.append((TOOL_SHAPES[k], {"M": MATERIAL[ti], "S": S_}, tool(ti, k), 1))
ARMOUR_SHAPES = [["MMM", "M M"], ["M M", "MMM", "MMM"], ["MMM", "M M", "M M"], ["M M", "M M"]]
ARMOUR_MAT = [T["hide"], T["iron bar"], T["gold bar"], T["gem"]]
for m in range(4):
    for p in range(4):
        recipes.append((ARMOUR_SHAPES[p], {"M": ARMOUR_MAT[m]}, armour(m, p), 1))

smelt = [(W.IRON, T["iron bar"]), (W.GOLD, T["gold bar"]), (W.SAND, W.GLASS), (W.RUBBLE, W.ROCK),
         (T["raw meat"], T["cooked meat"]), (T["clay lump"], T["brick"]), (W.LOG, T["coal"])]
fuel = [(T["coal"], 1600), (W.LOG, 300), (W.BOARDS, 300), (S_, 100), (W.BENCH, 300), (W.CHEST, 300),
        (W.FENCE, 300), (W.LADDER, 300), (W.DOOR, 200), (W.SAND, 0)]
fuel = [f for f in fuel if f[1]]

# ---- icons -------------------------------------------------------------------------------------
TIER_FILL = [PT.bayer(16), PT.bayer(34), PT.bayer(0), PT.bayer(52), PT.P("#.#.#.#.", ".#...#..", "#.#.#.#.", "...#...#",
                                                                      "#.#.#.#.", ".#...#..", "#.#.#.#.", "...#...#")]
MAT_FILL = [PT.bayer(24), PT.bayer(0), PT.bayer(52), TIER_FILL[4]]


def art(*rows, fill=None):
    """16 rows of 16: '#' black, '.' white, 'M' the fill pattern (black where it is), 'o' white
    with a black dot every other pixel (wood)."""
    assert len(rows) == 16, rows
    rows = [r.ljust(16, ".")[:16] for r in rows]
    out = []
    for y, r in enumerate(rows):
        v = 0
        for x, c in enumerate(r):
            b = 0
            if c == "#":
                b = 1
            elif c == "M":
                b = (fill[y & 7] >> (7 - (x & 7))) & 1 if fill else 1
            elif c == "o":
                b = (x + y) % 3 == 0
            v |= b << (15 - x)
        out.append(v)
    return out


def block_icon(t):
    """A little cube: the top face (a diamond) above the x side (left) and the z side (right)."""
    top, xs, zs = (PT.face_pattern(t, f, 0, 0) for f in (0, 1, 2))
    rows = []
    for y in range(16):
        v = 0
        for x in range(16):
            dx = x if x < 8 else 15 - x
            ytop, ylow, ybot = 4 - dx / 2, 4 + dx / 2, 12 + dx / 2
            b = 0
            if ytop <= y + 0.5 < ylow:
                b = (top[y & 7] >> (7 - (x & 7))) & 1
                if y == math.ceil(ytop - 0.5) or y == math.ceil(ylow - 0.5) - 1 and False:
                    b = 1
            elif ylow <= y + 0.5 < ybot:
                side = xs if x < 8 else zs
                b = (side[y & 7] >> (7 - (x & 7))) & 1
                if y == math.ceil(ylow - 0.5) or x in (0, 15) or x in (7, 8) and y >= 8 or y + 1.5 >= ybot or y == 15:
                    b = 1
            v |= b << (15 - x)
        rows.append(v)
    return rows


ICONS = {}
ICONS[TORCH] = art("................", "......##........", ".....#..#.......", ".....####.......", "......##........",
                   "......##........", "......##........", "......##........", "......##........", "......##........",
                   "......##........", "......##........", "......##........", "......##........", "......##........", "................")
ICONS[SAPLING] = art("................", ".......##.......", "......####......", ".....##.###.....", "....####.##.....",
                     "......####......", ".......##.......", ".......##.......", ".......##.......", "......####......",
                     ".....##..##.....", "................", "................", "................", "................", "................")
ICONS[T["stick"]] = art("................", "............##..", "...........##...", "..........##....", ".........##.....",
                        "........##......", ".......##.......", "......##........", ".....##.........", "....##..........",
                        "...##...........", "..##............", "................", "................", "................", "................")
ICONS[T["coal"]] = art("................", "................", ".....####.......", "....######......", "...#########....",
                       "..###########...", "..##########....", "..###########...", "...##########...", "....########....",
                       ".....######.....", "................", "................", "................", "................", "................")
BAR = ["................", "................", "................", "................", ".....########...",
       "....#MMMMMMM##..", "...#MMMMMMM#.#..", "..##########.#..", "..#MMMMMMMM#.#..", "..#MMMMMMMM#.#..",
       "..#MMMMMMMM##...", "..##########....", "................", "................", "................", "................"]
ICONS[T["iron bar"]] = art(*BAR, fill=TIER_FILL[2])
ICONS[T["gold bar"]] = art(*BAR, fill=TIER_FILL[3])
ICONS[T["gem"]] = art("................", "................", "....########....", "...#.#....#.#...", "..#..#....#..#..",
                      ".##############.", ".#....#..#....#.", "..#...#..#...#..", "...#...#.#..#...", "....#..#..#.#...",
                      ".....#.#.#.#....", "......#..#.#....", ".......##.......", "................", "................", "................")
ICONS[T["flint"]] = art("................", "................", "......###.......", ".....#####......", "....#######.....",
                        "...####.####....", "..#####.#####...", "..######.####...", "...##########...", "....########....",
                        "......####......", "................", "................", "................", "................", "................")
ICONS[T["clay lump"]] = art("................", "................", "................", ".....######.....", "....#......#....",
                            "...#..#.....#...", "...#.....#..#...", "...#........#...", "....#..#...#....", ".....######.....",
                            "................", "................", "................", "................", "................", "................")
ICONS[T["brick"]] = art("................", "................", "................", "....#########...", "...#.#.#.#.##...",
                        "..##########.#..", "..#.#.#.#.#.#...", "..#.#.#.#.#.#...", "..##########....", "................",
                        "................", "................", "................", "................", "................", "................")
ICONS[T["wheat"]] = art("................", "..#...#...#.....", ".###.###.###....", "..#...#...#.....", ".###.###.###....",
                        "..#...#...#.....", "...#..#..#......", "....#.#.#.......", ".....###........", "......#.........",
                        "......#.........", "......#.........", "................", "................", "................", "................")
ICONS[T["seeds"]] = art("................", "................", "................", "....#.....#.....", "...###...###....",
                        "....#..#..#.....", ".......###......", "..#.....#....#..", ".###........###.", "..#....#......#.",
                        "......###.......", ".......#........", "................", "................", "................", "................")
ICONS[T["bread"]] = art("................", "................", "................", ".....######.....", "...##......##...",
                        "..#..#..#..#.#..", ".#.#..#..#..#.#.", ".#............#.", ".#............#.", "..############..",
                        "................", "................", "................", "................", "................", "................")
ICONS[T["apple"]] = art("................", ".......#........", "........#.##....", "....###.###.....", "...#...#...#....",
                        "..#.........#...", "..#.##......#...", "..#.#.......#...", "..#.........#...", "...#.......#....",
                        "....###.###.....", "................", "................", "................", "................", "................")
MEAT = ["................", "................", "....######......", "...#MMMMMM#.....", "..#MMMMMMMM#....",
        "..#MMM##MMM#....", "..#MMMMMMMMM#...", "...#MMMMMMM#.##.", "....######.#..#", "...........#..#",
        "............##.", "................", "................", "................", "................", "................"]
ICONS[T["raw meat"]] = art(*[r[:16] for r in MEAT], fill=PT.bayer(20))
ICONS[T["cooked meat"]] = art(*[r[:16] for r in MEAT], fill=PT.bayer(48))
ICONS[T["hide"]] = art("................", "................", "...##.....##....", "...#M#####M#....", "...#MMMMMMM#....",
                       "....#MMMMM#.....", "....#MMMMM#.....", "....#MMMMM#.....", "...#MMMMMMM#....", "...#M#####M#....",
                       "...##.....##....", "................", "................", "................", "................", "................",
                       fill=PT.bayer(24))
ICONS[T["feather"]] = art("................", "...........##...", ".........##.#...", "........#..##...", ".......#.#.#....",
                          "......#.#..#....", ".....#.#..#.....", "....#.#..#......", "....##..#.......", "...#.###........",
                          "..#.............", ".#..............", "................", "................", "................", "................")
ICONS[T["string"]] = art("................", "................", "..##............", "...#............", "....#...........",
                         ".....##.........", ".......##.......", ".........#......", "..........#.....", "...........##...",
                         ".............#..", "................", "................", "................", "................", "................")
ICONS[T["arrow"]] = art("................", "...........###..", "............##..", "...........#.#..", "..........#.....",
                        ".........#......", "........#.......", ".......#........", "......#.........", ".##..#..........",
                        ".#.##...........", "..##............", ".#.#............", "................", "................", "................")
ICONS[T["bow"]] = art("................", ".....######.....", "....#......#....", "...#........#...", "...#........#...",
                      "..#..........#..", "..#..........#..", "..#..........#..", "..#..........#..", "..##############",
                      "................", "................", "................", "................", "................", "................")
HEADS = [
    ["..MMMMMMMMMMMM..", ".MM....##....MM.", "M......##......M", ".......##.......", ".......##.......",
     ".......##.......", ".......##.......", ".......##.......", ".......##.......", ".......##.......",
     ".......##.......", ".......##.......", ".......##.......", "................", "................", "................"],
    ["................", "...MMMMM........", "..MMMMMMM##.....", "..MMMMMMM##.....", "...MMMMM.##.....",
     ".........##.....", ".........##.....", ".........##.....", ".........##.....", ".........##.....",
     ".........##.....", ".........##.....", ".........##.....", "................", "................", "................"],
    ["................", "......MMMM......", "......MMMM......", "......MMMM......", "......MMMM......",
     ".......##.......", ".......##.......", ".......##.......", ".......##.......", ".......##.......",
     ".......##.......", "......####......", "......#..#......", "................", "................", "................"],
    ["................", "...........MM...", "..........MMM...", ".........MMM....", "........MMM.....",
     ".......MMM......", "......MMM.......", ".....MMM........", "..#.MMM.........", "...###..........",
     "....##..........", "...#..#.........", "..#.............", "................", "................", "................"],
    ["................", "...MMMMMMM......", "...MMMMMMM##....", ".........##.....", ".........##.....",
     ".........##.....", ".........##.....", ".........##.....", ".........##.....", ".........##.....",
     ".........##.....", ".........##.....", ".........##.....", "................", "................", "................"],
]
for ti in range(5):
    fill = TIER_FILL[ti]
    for k in range(5):
        rows = [r.replace("MM", "MM") for r in HEADS[k]]
        ic = art(*rows, fill=fill)
        # an outline round the head so light tiers still show
        hm = art(*[r.replace("#", ".").replace("M", "#") for r in HEADS[k]])
        out = []
        for y in range(16):
            o = 0
            for x in range(16):
                inside = (hm[y] >> (15 - x)) & 1
                if inside:
                    edge = any(not (0 <= x + dx < 16 and 0 <= y + dy < 16) or not ((hm[y + dy] >> (15 - x - dx)) & 1)
                               for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
                    if edge:
                        o |= 1 << (15 - x)
            out.append(ic[y] | o)
        ICONS[tool(ti, k)] = out
ARM = [
    ["................", "................", "....########....", "...#MMMMMMMM#...", "..#MMMMMMMMMM#..",
     "..#MMMMMMMMMM#..", "..#MM######MM#..", "..#M#......#M#..", "..##........##..", "................",
     "................", "................", "................", "................", "................", "................"],
    ["................", "..###.....###...", ".#MMM#####MMM#..", ".#MMMMMMMMMMM#..", ".#MMMMMMMMMMM#..",
     "..##MMMMMMM##...", "....#MMMMM#.....", "....#MMMMM#.....", "....#MMMMM#.....", "....#MMMMM#.....",
     "....#######.....", "................", "................", "................", "................", "................"],
    ["................", "...##########...", "...#MMMMMMMM#...", "...#MMMMMMMM#...", "...#MMM##MMM#...",
     "...#MMM##MMM#...", "...#MMM##MMM#...", "...#MMM##MMM#...", "...#MMM##MMM#...", "...#####.####...",
     "................", "................", "................", "................", "................", "................"],
    ["................", "................", "................", "...####..####...", "...#MM#..#MM#...",
     "...#MM#..#MM#...", "...#MM#..#MM#...", "..#MMM#..#MMM#..", ".#MMMM#..#MMMM#.", ".######..######.",
     "................", "................", "................", "................", "................", "................"],
]
for m in range(4):
    for p in range(4):
        ICONS[armour(m, p)] = art(*ARM[p], fill=MAT_FILL[m])
for t in range(1, 29):
    if names[t] and t not in ICONS:
        ICONS[t] = block_icon(t)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    h = ["/* generated by app/gen_items.py: Blockworld items */", "#ifndef BW_ITEMS_H", "#define BW_ITEMS_H",
         f"#define NITEMS {NITEMS}", f"#define I_TORCH {TORCH}", f"#define I_SAPLING {SAPLING}"]
    for n, i in T.items():
        h.append(f"#define I_{n.upper().replace(' ', '_')} {i}")
    h += ["#define I_TOOL(tier, kind) (64 + (tier) * 5 + (kind))",
          "#define I_ARMOUR(mat, piece) (96 + (mat) * 4 + (piece))",
          "#define IS_TOOL(i) ((i) >= 64 && (i) < 89)", "#define IS_ARMOUR(i) ((i) >= 96 && (i) < 112)",
          "#define TOOL_TIER(i) (((i) - 64) / 5)", "#define TOOL_KIND(i) (((i) - 64) % 5)",
          "enum { K_PICK, K_AXE, K_SHOVEL, K_SWORD, K_HOE };",
          "enum { MT_ANY, MT_PICK, MT_AXE, MT_SHOVEL };",
          "typedef struct Recipe { unsigned char w, h, out, count; unsigned char key[9]; } Recipe;",
          "typedef struct Mine { unsigned char hard, tool; signed char level; unsigned char drop, count; } Mine;",
          "extern const char *const item_name[NITEMS];",
          "extern const unsigned char item_stack[NITEMS], item_food[NITEMS], item_armour[NITEMS];",
          "extern const unsigned short item_dur[NITEMS];",
          "extern const unsigned short item_icon[NITEMS][16];",
          f"#define NRECIPES {len(recipes)}", "extern const Recipe recipes[NRECIPES];",
          f"#define NSMELT {len(smelt)}", "extern const unsigned char smelt_in[NSMELT], smelt_out[NSMELT];",
          f"#define NFUEL {len(fuel)}", "extern const unsigned char fuel_item[NFUEL];",
          "extern const unsigned short fuel_ticks[NFUEL];",
          "extern const Mine block_mine[32];",
          "extern const unsigned char tool_speed[5], tier_level[5];",
          "#endif"]
    open(os.path.join(out, "items.h"), "w").write("\n".join(h) + "\n")
    c = ["/* generated by app/gen_items.py */", '#include "items.h"']
    c.append("const char *const item_name[NITEMS] = {" + ", ".join(f'"{n}"' for n in names) + "};")
    c.append("const unsigned char item_stack[NITEMS] = {" + ", ".join(map(str, stack)) + "};")
    c.append("const unsigned char item_food[NITEMS] = {" + ", ".join(map(str, food)) + "};")
    c.append("const unsigned char item_armour[NITEMS] = {" + ", ".join(map(str, armour_def)) + "};")
    c.append("const unsigned short item_dur[NITEMS] = {" + ", ".join(map(str, dur)) + "};")
    c.append("const unsigned short item_icon[NITEMS][16] = {")
    for i in range(NITEMS):
        ic = ICONS.get(i, [0] * 16)
        c.append("    {" + ", ".join(f"0x{v:04X}" for v in ic) + "},")
    c.append("};")
    c.append("const Recipe recipes[NRECIPES] = {")
    for rows, key, o, n in recipes:
        w = max(len(r) for r in rows)
        cells = []
        for r in rows:
            r = r.ljust(w)
            cells += [key.get(ch, 0) if ch != " " else 0 for ch in r]
        cells += [0] * (9 - len(cells))
        c.append(f"    {{{w}, {len(rows)}, {o}, {n}, {{{', '.join(map(str, cells))}}}}},")
    c.append("};")
    c.append("const unsigned char smelt_in[NSMELT] = {" + ", ".join(str(a) for a, b in smelt) + "};")
    c.append("const unsigned char smelt_out[NSMELT] = {" + ", ".join(str(b) for a, b in smelt) + "};")
    c.append("const unsigned char fuel_item[NFUEL] = {" + ", ".join(str(a) for a, b in fuel) + "};")
    c.append("const unsigned short fuel_ticks[NFUEL] = {" + ", ".join(str(b) for a, b in fuel) + "};")
    c.append("const Mine block_mine[32] = {")
    for t in range(32):
        hd, tl, lv, dr, cn = mine.get(t, (0, 0, 99, 0, 0))
        c.append(f"    {{{hd}, {tl}, {lv if lv < 99 else 99}, {dr}, {cn}}},")
    c.append("};")
    c.append("const unsigned char tool_speed[5] = {" + ", ".join(map(str, TOOL_SPEED)) + "};")
    c.append("const unsigned char tier_level[5] = {" + ", ".join(map(str, TIER_LEVEL)) + "};")
    open(os.path.join(out, "items.c"), "w").write("\n".join(c) + "\n")
    if "--png" in sys.argv:
        from PIL import Image
        ids = [i for i in range(NITEMS) if names[i]]
        cols = 16
        im = Image.new("L", (cols * 20, ((len(ids) + cols - 1) // cols) * 20), 255)
        for k, i in enumerate(ids):
            ox, oy = (k % cols) * 20 + 2, (k // cols) * 20 + 2
            for y in range(16):
                for x in range(16):
                    if (ICONS[i][y] >> (15 - x)) & 1:
                        im.putpixel((ox + x, oy + y), 0)
        im.resize((im.width * 3, im.height * 3), Image.NEAREST).save(sys.argv[sys.argv.index("--png") + 1])
    print(f"{len([n for n in names if n])} items, {len(recipes)} recipes, {len(smelt)} smelting, {len(fuel)} fuels")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Builds runner/art/sprites.txt from the ASCII part library below.

Characters are assembled from parts (head, torso, arms, legs, shoes, props) so every frame of a
character keeps the same proportions. Pixel characters are those of tools/sprites.py:
' ' transparent, '#' black, '.' white, '+' 50% grey, ':' 25% grey.

Placement coordinates are given in a "design box" (for Rex 32x48: the body is centred between
x=15 and x=16 and the ground is row 47). Every frame is drawn on a padded canvas and trimmed, so
the anchor written to sprites.txt always points at the same body-relative ground point.

    python3 compose.py          -> writes sprites.txt
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))

P = {}


# ---------------------------------------------------------------- small raster utilities
def flip(rows):
    w = max(len(r) for r in rows)
    return [r.ljust(w)[::-1].rstrip() for r in rows]


def rot180(rows):
    w = max(len(r) for r in rows)
    return [r.ljust(w)[::-1] for r in rows[::-1]]


def ellipse(w, h, shrink=0.25, fill="."):
    """Filled ellipse w x h with a 1 px black outline (4-connected edge)."""
    cx, cy = (w - 1) / 2, (h - 1) / 2
    rx, ry = w / 2 - shrink, h / 2 - shrink
    ins = [[((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1 for x in range(w)] for y in range(h)]
    rows = []
    for y in range(h):
        r = ""
        for x in range(w):
            if not ins[y][x]:
                r += " "
                continue
            edge = any(not (0 <= x + dx < w and 0 <= y + dy < h and ins[y + dy][x + dx])
                       for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
            r += "#" if edge else fill
        rows.append(r)
    return rows


def overlay(base, part, x, y, clip=True):
    """Paint part onto a copy of base; with clip, only over base's white interior."""
    w = max(max(len(r) for r in base), max(x + len(r) for r in part) if not clip else 0)
    out = [list(r.ljust(w)) for r in base]
    for j, r in enumerate(part):
        for i, ch in enumerate(r):
            yy, xx = y + j, x + i
            if ch == " " or not (0 <= yy < len(out)) or not (0 <= xx < len(out[yy])):
                continue
            if clip and out[yy][xx] != ".":
                continue
            out[yy][xx] = ch
    return ["".join(r).rstrip() for r in out]


# ---------------------------------------------------------------- Rex parts (seen from behind)
# head: backwards cap (black crown with a white R on the front panel, which now faces us; the
# white brim points at the camera), black hair under the brim
P["head"] = [
    "     ######     ",
    "   ##########   ",
    "  ####...#####  ",
    " #####.##.##### ",
    " #####...###### ",
    " #####.#.###### ",
    " #####.##.##### ",
    " ############## ",
    " #............# ",
    "#..............#",
    "#..............#",
    " ##..........## ",
    "################",
]
P["head_nocap"] = [
    "  #  ##  #  #   ",
    "  ## ### ## ##  ",
    " ############## ",
    " ############## ",
    "################",
    "################",
    "################",
    "################",
    "#.############.#",
    "#.############.#",
    " ############## ",
    "  ############  ",
    "   ##########   ",
]
# long-sleeved tee with a big star on the back
P["tee"] = [
    "    ##############    ",
    "  ##..............##  ",
    " #.........##.......# ",
    " #.........##.......# ",
    " #.....##########...# ",
    " #.......######.....# ",
    " #........####......# ",
    " #.......##..##.....# ",
    " #.......#....#.....# ",
    " #..................# ",
    " #..................# ",
    " #..................# ",
    "  ##################  ",
]
P["shorts"] = [
    "################",
    "#.############.#",
    "#.############.#",
    "#.#####  #####.#",
]
P["leg"] = [
    "#...#",
    "#...#",
    "#...#",
    "#...#",
    "#...#",
    "#...#",
    "#...#",
    "#####",
    "#...#",
    "#...#",
    "#...#",
]
P["leg_short"] = P["leg"][2:]
P["thigh"] = [
    "#...#",
    "#...#",
    "#...#",
]
P["leg_push"] = [
    "#...#",
    "#....#",
    " #....#",
    "  #....#",
    "   #....#",
    "    #...#",
    "    #...#",
]
# sneakers: heel view (planted), tilted (pushing off), sole (kicked up toward the camera)
P["shoe_heel"] = [
    "  #####  ",
    " #.....# ",
    "#..###..#",
    "#...#...#",
    "#.......#",
    "#########",
    " ####### ",
]
P["shoe_mid"] = [
    "  #####  ",
    " #.....# ",
    "#..###..#",
    "#########",
    "#.#####.#",
    "#.......#",
    " ####### ",
]
P["shoe_sole"] = [
    "  #####  ",
    " #.....# ",
    " #.###.# ",
    " #.....# ",
    "  #...#  ",
    "  #...#  ",
    " #.....# ",
    "#.......#",
    "#.#####.#",
    "#.......#",
    "#.#####.#",
    " #.....# ",
    "  #####  ",
]
P["sole_small"] = [
    " ##### ",
    "#.....#",
    "#.###.#",
    "#.....#",
    "#.###.#",
    "#.....#",
    " ##### ",
]
# left arm (screen left) swung back: fist low and out
P["arm_back"] = [
    "     ###",
    "    #..#",
    "   #...#",
    "   #...#",
    "  #...# ",
    "  #...# ",
    " #...#  ",
    " #...#  ",
    " #####  ",
    "#....#  ",
    "#....#  ",
    " ####   ",
]
# left arm swung forward: only the upper arm and elbow show
P["arm_fwd"] = [
    "     ###",
    "    #..#",
    "   #...#",
    "   #...#",
    "   #...#",
    "   #...#",
    "    ###",
]
# right arm raised with the elbow out (the can sits on top of the fist)
P["arm_up"] = [
    "      #####",
    "     #.....#",
    "     #.....#",
    "      #####",
    "      #...#",
    "      #...#",
    "###   #...#",
    "#..#  #...#",
    "#...# #...#",
    "#....##...#",
    " #.........#",
    "  #........#",
    "   #......#",
    "    ######",
]
# arm held out sideways (right side), fist at the end
P["arm_out"] = [
    "         #####",
    "#########.....#",
    "#.......#.....#",
    "#.......#.....#",
    "#########.....#",
    "         #####",
]
# arm reaching up and out at 45 degrees (right side), fist top right
P["arm_diag"] = [
    "        ####",
    "       #....#",
    "       #....#",
    "      ##....#",
    "     #..####",
    "    #...#",
    "   #...#",
    "  #...#",
    " #...#",
    "#...#",
    "#..#",
]
P["arm_up_l"] = flip(P["arm_up"])
P["arm_out_l"] = flip(P["arm_out"])
# the spray can: white cap with nozzle, black can with a highlight
P["can"] = [
    " ### ",
    "#...#",
    "#...#",
    "#####",
    "#.###",
    "#.###",
    "#.###",
    "#####",
]
P["sparkle"] = [
    "  #  ",
    "  #  ",
    "##.##",
    "  #  ",
    "  #  ",
]
# jetpack: two black tanks side by side with white highlights and nozzles
P["jetpack"] = [
    " ####  #### ",
    "#....##....#",
    "#....##....#",
    "############",
    "#.####.#####",
    "#.####.#####",
    "#.####.#####",
    "#.####.#####",
    "#.####.#####",
    "#.####.#####",
    "#.####.#####",
    "############",
    " #..#  #..# ",
    "#....##....#",
    "############",
]
P["flame0"] = [
    "#....#",
    "#.##.#",
    "#.##.#",
    "#.##.#",
    " #..# ",
    "#.##.#",
    " #..# ",
    " #..# ",
    "  ##  ",
    "  #.# ",
    "   #  ",
]
P["flame1"] = [
    "#....#",
    "#.##.#",
    "#.##.#",
    " #..# ",
    " #.#  ",
    "  ##  ",
    " #  # ",
    "#    #",
]
P["board"] = [
    "   ##############################   ",
    " ##..............................## ",
    "#.....##########################.....#",
    "#....................................#",
    " ##..............................## ",
    "   ##############################   ",
    "     ##########################     ",
]
P["hover0"] = [
    "     ####################     ",
    "  ######     ######     ######",
    "    ##   ##          ##   ##  ",
]
P["hover1"] = [
    "     ####################     ",
    "    ####     ######     ####  ",
    "  ##       ##      ##       ##",
]

# ---------------------------------------------------------------- composition helpers
FRAMES = {}
ORDER = []
PAD = 24          # canvas margin around the design box


def frame(name, w, h, ax, ay, ops):
    FRAMES[name] = (w, h, ax, ay, ops)
    ORDER.append(name)


def blit(c, part, x, y, flip_x=False, halo=False):
    """Paint part at (x, y). With halo, first turn black canvas pixels that touch the part's
    silhouette (4-neighbours) white, so a black limb stays separate from a black body."""
    rows = P[part] if isinstance(part, str) else part
    pw = max(len(r) for r in rows)
    rows = [r.ljust(pw)[::-1] if flip_x else r.ljust(pw) for r in rows]
    H, W = len(c), len(c[0])
    if halo:
        for j, r in enumerate(rows):
            for i, ch in enumerate(r):
                if ch == " ":
                    continue
                for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    ii, jj = i + di, j + dj
                    inside = 0 <= jj < len(rows) and 0 <= ii < pw and rows[jj][ii] != " "
                    yy, xx = y + jj, x + ii
                    if not inside and 0 <= yy < H and 0 <= xx < W and c[yy][xx] == "#":
                        c[yy][xx] = "."
    for j, r in enumerate(rows):
        for i, ch in enumerate(r):
            if ch != " " and 0 <= y + j < H and 0 <= x + i < W:
                c[y + j][x + i] = ch


def render(name):
    """Returns (rows, ax, ay), trimmed to the opaque bounding box (anchor adjusted)."""
    w, h, ax, ay, ops = FRAMES[name][:5]
    notrim = "notrim" in FRAMES[name][5:]
    c = [[" "] * w for _ in range(h)]
    for op in ops:
        blit(c, *op)
    rows = ["".join(r).rstrip() for r in c]
    if notrim:
        while rows and not rows[-1]:
            rows.pop()
        return rows, ax, ay
    origin = "origin" in FRAMES[name][5:]
    while rows and not rows[0]:
        rows.pop(0)
        ay -= 1
    while rows and not rows[-1]:
        rows.pop()
    left = min(len(r) - len(r.lstrip()) for r in rows if r)
    rows = [r[left:] for r in rows]
    ax -= left
    for i, r in enumerate(rows):
        if not r:
            raise SystemExit(f"{name}: blank interior row {i}")
    if origin:
        ax = ay = 0
    return rows, ax, ay


def design(name, ops, ax=16, ay=47, box=(32, 48)):
    """Frame from ops in design-box coordinates (shifted onto a padded canvas)."""
    bw, bh = box
    frame(name, bw + 2 * PAD, bh + 2 * PAD, ax + PAD, ay + PAD,
          [(o[0], o[1] + PAD, o[2] + PAD) + tuple(o[3:]) for o in ops])


def sheared(name, ops, hip, k, box=(32, 48)):
    """Render design-coordinate ops, shift each row above design row `hip` sideways by
    round((hip - y) * k), store the result as a part and return an op that places it."""
    W, H = box[0] + 2 * PAD, box[1] + 2 * PAD
    c = [[" "] * W for _ in range(H)]
    for o in ops:
        blit(c, o[0], o[1] + PAD, o[2] + PAD, *o[3:])
    for y in range(H):
        d = round((hip - (y - PAD)) * k) if y - PAD < hip else 0
        if d > 0:
            c[y] = [" "] * d + c[y][:W - d]
        elif d < 0:
            c[y] = c[y][-d:] + [" "] * (-d)
    P[name] = ["".join(r) for r in c]
    return (name, -PAD, -PAD)


# ---------------------------------------------------------------- Rex frames
def rex(name, legs, larm, rarm="can", up=0, shear=0.0, extra=(), under=(), head="head", ay=47):
    """Rex from behind. legs: ops in design coords (drawn first). larm/rarm pick arm poses.
    up lifts the upper body; shear leans everything above the hips (px per row, + = right)."""
    u = 1 - up
    ops = [("shorts", 8, 27 + u)]
    if larm == "fwd":
        ops.append(("arm_fwd", 1, 15 + u))
    if larm == "out":
        ops.append(("arm_out_l", -8, 15 + u))
    if larm == "diag":
        ops.append(("arm_diag", -3, 7 + u, True))
    if rarm == "out":
        ops.append(("arm_out", 21, 15 + u))
    if rarm == "canout":
        ops.append(("arm_out", 21, 15 + u))
        ops.append(("can", 30, 8 + u))
    if rarm in ("diag", "candiag"):
        ops.append(("arm_diag", 22, 7 + u))
    if rarm == "candiag":
        ops.append(("can", 30, -1 + u))
    ops.append(("tee", 5, 14 + u))
    if rarm == "can":
        ops.append(("arm_up", 20, 9 + u))
        ops.append(("can", 26, 2 + u))
    if rarm == "canhi":
        ops.append(("arm_up", 20, 6 + u))
        ops.append(("can", 26, -1 + u))
    if larm == "up":
        ops.append(("arm_up_l", 0, 9 + u))
    if larm == "hi":
        ops.append(("arm_up_l", 0, 6 + u))
    if head:
        ops.append((head, 8, 1 + u))
    if larm == "back":
        ops.append(("arm_back", 0, 15 + u))
    ops += list(extra)
    if shear:
        ops = [sheared("_" + name, ops, 28 + u, shear)]
    design(name, list(under) + list(legs) + ops, ay=ay)


RUN0_LEGS = [("leg", 9, 31), ("shoe_heel", 7, 41), ("thigh", 18, 31), ("shoe_sole", 16, 31)]
RUN2_LEGS = [("leg", 18, 31), ("shoe_heel", 16, 41), ("thigh", 9, 31), ("shoe_sole", 7, 31)]
rex("rex_run0", RUN0_LEGS, "back")
rex("rex_run1", [("leg", 9, 30), ("shoe_mid", 7, 41),
                 ("leg_short", 18, 30), ("shoe_heel", 16, 37)], "fwd", up=1)
rex("rex_run2", RUN2_LEGS, "fwd")
rex("rex_run3", [("leg", 18, 30), ("shoe_mid", 16, 41),
                 ("leg_short", 9, 30), ("shoe_heel", 7, 37)], "back", up=1)

rex("rex_jump", [("thigh", 9, 27), ("thigh", 18, 27),
                 ("shoe_sole", 6, 26), ("shoe_sole", 17, 26)], "hi", "canhi", up=4)
rex("rex_fall", [("leg", 8, 29), ("leg", 19, 29), ("leg_short", 8, 31), ("leg_short", 19, 31),
                 ("shoe_heel", 6, 39), ("shoe_heel", 17, 39)], "diag", "candiag", up=3)
rex("rex_left", [("leg", 8, 31), ("shoe_heel", 6, 41),
                 ("leg_push", 18, 31), ("shoe_mid", 20, 38)], "back", "can", shear=-0.2)
rex("rex_right", [("leg", 19, 31), ("shoe_heel", 17, 41),
                  ("leg_push", 9, 31, True), ("shoe_mid", 3, 38)], "up", "canout", shear=0.2)
rex("rex_stumble", [("leg", 8, 31), ("shoe_heel", 6, 41),
                    ("thigh", 19, 31), ("shoe_sole", 18, 30)], "diag", "candiag", shear=0.15,
    extra=[("sparkle", 3, 0), ("sparkle", 26, 2)])

# jetpack: twin tanks on the back, legs dangling apart, a flame under each nozzle
JET_LEGS = [("leg", 5, 30), ("leg", 22, 30), ("shoe_heel", 3, 38), ("shoe_heel", 20, 38)]
for i in (0, 1):
    rex(f"rex_jet{i}", JET_LEGS, "back", "can", up=3,
        extra=[("jetpack", 10, 12), (f"flame{i}", 10, 27), (f"flame{i}", 16, 27)])

# hoverboard: wide stance, arms out for balance, board under the feet, hover lines below
for i in (0, 1):
    rex(f"rex_board{i}", [("leg_push", 4, 30, True), ("leg_push", 19, 30),
                          ("shoe_heel", 0, 36), ("shoe_heel", 23, 36),
                          ("board", -3, 41), (f"hover{i}", 1, 47)],
        "out", "canout", up=1 - i, ay=49)

# roll: curled into a ball, two rotation frames
BALL = ellipse(26, 24)
BALL = overlay(BALL, P["head"][:8], 5, 0)
BALL = overlay(BALL, ["#" * 26], 0, 8)
BALL = overlay(BALL, P["tee"][2:9], 2, 9)
BALL = overlay(BALL, ["#" * 26] * 3, 0, 16)
BALL = overlay(BALL, P["sole_small"], 5, 17, clip=False)
BALL = overlay(BALL, P["sole_small"], 14, 17, clip=False)
P["ball0"] = BALL
P["ball1"] = rot180(BALL)
design("rex_roll0", [("ball0", 3, 24)])
design("rex_roll1", [("ball1", 3, 24)])

# crash: spread-eagle splat, cap knocked off
P["cap_fly"] = P["head"][:12]
rex("rex_crash", [("leg_push", 18, 28), ("leg_push", 9, 28, True),
                  ("sole_small", 21, 32), ("sole_small", 3, 32)],
    "diag", "diag", head="head_nocap",
    extra=[("cap_fly", 22, -7), ("sparkle", 3, -2), ("sparkle", 30, 9)])


# ---------------------------------------------------------------- Officer Grumble (from behind)
def sym(halves):
    """Mirror left-half rows into full symmetric rows."""
    return [h + h[::-1] for h in halves]


# peaked police cap (white top, checkered band), stubbly back of the head, jug ears, thick neck
P["g_head"] = sym([
    "   ########",
    "  #........",
    " #.........",
    " ##########",
    "  #########",
    "  #########",
    "  #.##..##.",
    "  #.##..##.",
    "  ##..##..#",
    "  ##..##..#",
    "  #########",
    "  #########",
    "###########",
    "#.#########",
    "#..########",
    "#..#.######",
    " ##....####",
    "   #.....##",
    "   #.......",
    "   #.......",
])
# guard jacket: black, white epaulettes and collar, a white GUARD patch and one reflective stripe
P["g_jacket"] = sym([
    "             #########",
    "        #####.........",
    "     ########.........",
    "   ###################",
    "  ####......##########",
    " #####......##########",
    " #####################",
    "######################",
    "##########............",
    "##########............",
    "##########............",
    "##########............",
    "##########............",
    "##########............",
    "##########............",
    "######################",
    "######################",
    "#.....................",
    "#.....................",
    "#.....................",
    "######################",
    "######################",
    "######################",
    " #####################",
    " #####################",
])
FONT3 = {
    "G": [".##", "#..", "#.#", "#.#", ".##"],
    "U": ["#.#", "#.#", "#.#", "#.#", "###"],
    "A": [".#.", "#.#", "###", "#.#", "#.#"],
    "R": ["##.", "#.#", "##.", "#.#", "#.#"],
    "D": ["##.", "#.#", "#.#", "#.#", "##."],
}


def text3(word):
    """Black 3x5 letters on opaque white, 1 px apart."""
    return [".".join(FONT3[ch][r] for ch in word) for r in range(5)]


P["g_guard"] = text3("GUARD")
P["g_belt"] = sym([
    "####################",
    "#...................",
    "#..####.............",
    "#..####.............",
    "####################",
])
P["g_leg"] = ["#" * 14] * 12
P["g_thigh"] = ["#" * 14] * 4
P["g_boot"] = [
    " ############## ",
    "################",
    "################",
    "################",
    "################",
    "#..............#",
    "################",
    " ############## ",
]
P["g_sole"] = [
    "  ############  ",
    " #............# ",
    "#..##########..#",
    "#..............#",
    "#..##########..#",
    "#..............#",
    " #............# ",
    "  #..........#  ",
    "  #..######..#  ",
    "  #..........#  ",
    "   ##########   ",
]
# left arm hanging (black sleeve, reflective bands, white fist)
P["g_arm_down"] = [
    "    ######",
    "  ########",
    " #########",
    " #########",
    "##########",
    "##########",
    "##########",
    "#........#",
    "#........#",
    "##########",
    "##########",
    "##########",
    "######### ",
    "######### ",
    "######### ",
    "#.......# ",
    "#.......# ",
    "######### ",
    " ######## ",
    "#.......# ",
    "#.#.#...# ",
    "#.#.#...# ",
    "#.......# ",
    " #######  ",
]
# left arm swung forward: upper arm only
P["g_arm_fwd"] = [
    "    ######",
    "  ########",
    " #########",
    " #########",
    "##########",
    "##########",
    "##########",
    "#........#",
    "#........#",
    "##########",
    "##########",
    "##########",
    " ######## ",
    "  ######  ",
]
# right arm raised, elbow out at shoulder height, fist shaking above
P["g_arm_raise"] = [
    "         ########  ",
    "        #........# ",
    "        #.#.#....# ",
    "        #.#.#....# ",
    "        #........# ",
    "        #........# ",
    "         ########  ",
    "          ######   ",
    "          ######   ",
    "         #......#  ",
    "         #......#  ",
    "         ########  ",
    "         ########  ",
    "         ########  ",
    "  ################ ",
    " ##################",
    " ##################",
    " ##################",
    "  ################ ",
    "   ############### ",
]
# arm reaching forward and up with an open hand (right side; flipped for the left)
P["g_arm_reach"] = [
    "  #  #  #      ",
    " #.##.##.#     ",
    " #.##.##.#     ",
    " #.##.##.#     ",
    "##.......#     ",
    "#.#......#     ",
    " #.......#     ",
    "  #######      ",
    "  ########     ",
    "  ########     ",
    "  #......#     ",
    "  #......#     ",
    "  #########    ",
    "   #########   ",
    "   ##########  ",
    "    ########## ",
    "    ########## ",
    "     ##########",
    "     ##########",
]
P["shake"] = [
    "##",
    "  ",
    "# ",
]


def grumble(name, legs, larm, rarm, extra=(), dy=0):
    """Officer Grumble from behind in a 56x70 design box (centre x 27.5, ground row 66)."""
    ops = list(legs)
    ops.append(("g_belt", 8, 43 + dy))
    ops.append(("g_jacket", 6, 18 + dy))
    ops.append(("g_guard", 6 + 12, 18 + 9 + dy))
    ops.append(("g_head", 17, 0 + dy))
    if larm == "down":
        ops.append(("g_arm_down", 1, 20 + dy, False, True))
    if larm == "fwd":
        ops.append(("g_arm_fwd", 1, 20 + dy, False, True))
    if larm == "reach":
        ops.append(("g_arm_reach", 3, 3 + dy, True, True))
    if rarm == "raise":
        ops.append(("g_arm_raise", 39, 6 + dy, False, True))
    if rarm == "raise2":
        ops.append(("g_arm_raise", 37, 9 + dy, False, True))
    if rarm == "reach":
        ops.append(("g_arm_reach", 38, 3 + dy, False, True))
    ops += list(extra)
    design(name, ops, ax=28, ay=66, box=(56, 70))


grumble("grumble_run0", [("g_leg", 13, 47), ("g_boot", 12, 59),
                         ("g_thigh", 29, 47), ("g_sole", 28, 48)], "down", "raise",
        extra=[("shake", 54, 4), ("shake", 36, 3, True)])
grumble("grumble_run1", [("g_leg", 29, 47), ("g_boot", 28, 59),
                         ("g_thigh", 13, 47), ("g_sole", 12, 48)], "fwd", "raise2", dy=-1,
        extra=[("shake", 54, 9), ("shake", 34, 7, True)])
grumble("grumble_grab", [("g_leg", 13, 47), ("g_boot", 12, 59),
                         ("g_leg", 29, 47), ("g_boot", 28, 59)], "reach", "reach", dy=2)



# ---------------------------------------------------------------- Biscuit the dog (from behind)
# floppy black ears, collar, a spot on one haunch, wagging tail, hind legs (one paw lifted)
P["biscuit0"] = [
    "          ####",
    "         #....#",
    "       ##......##",
    "      #..........#",
    "    ###..........###",
    "   ####..........####",
    "   ####..........####",
    "   #####........#####",
    "   ######......######",
    "    #### ###### ####",
    "     ##  #....#  ##    ##",
    "        #......#     #..#",
    "       #........#   #..#",
    "      #..........# #..#",
    "     #............#..#",
    "     #.............#.#",
    "     #..##..........#",
    "     #.####.........#",
    "     #.####..##.....#",
    "     #..##..#  #....#",
    "     #......#  ##.#.#",
    "     #.#.#.#    #.##",
    "      #####      ##",
]
P["biscuit1"] = [
    "          ####",
    "         #....#",
    "       ##......##",
    "      #..........#",
    "    ###..........###",
    "   ####..........####",
    "   ####..........####",
    "   #####........#####",
    "   ######......######",
    "    #### ###### ####",
    "##   ##  #....#  ##",
    "#..#    #......#",
    " #..#  #........#",
    "  #..##..........#",
    "   #..#...........#",
    "    ##.............#",
    "     #..##..........#",
    "     #.####.........#",
    "     #.####..##.....#",
    "     ##.##..#  #.....#",
    "      #.#.##   #.....#",
    "       ##     #.#.#.#",
    "               #####",
]
design("biscuit_run0", [("biscuit0", 0, 0)], ax=13, ay=22, box=(26, 23))
design("biscuit_run1", [("biscuit1", 0, 0)], ax=13, ay=22, box=(26, 23))



# ---------------------------------------------------------------- power-up bubbles (24x24)
def ring(d, t=2):
    """Bubble: d x d disc, white inside, black ring t px thick."""
    outer = ellipse(d, d)
    inner = ellipse(d - 2 * (t - 1), d - 2 * (t - 1))
    rows = [list(r.ljust(d)) for r in outer]
    for y in range(d):
        for x in range(d):
            if rows[y][x] == ".":
                rows[y][x] = "#"
    for j, r in enumerate(inner):
        for i, ch in enumerate(r):
            if ch == ".":
                rows[j + t - 1][i + t - 1] = "."
    return ["".join(r).rstrip() for r in rows]


P["bubble"] = ring(24, 2)

P["i_jetpack"] = [
    "  #      #",
    " #.#    #.#",
    "#...#  #...#",
    "#####  #####",
    "#.##########",
    "#.####.#####",
    "#.####.#####",
    "#.####.#####",
    "#####  #####",
    " #.#    #.#",
    "#...#  #...#",
    "#.#.#  #.#.#",
    " #.#    #.#",
    "  #      #",
]
P["i_sneakers"] = [
    "     #####        ",
    "     #####        ",
    "     ##.###       ",
    "###  ###.###      ",
    "     ##.#.###     ",
    "###  ###.#.#####  ",
    "     ############ ",
    " ##  #######...## ",
    "     ######.....##",
    "    ##############",
    "    #............#",
    "     ############ ",
]
P["i_magnet"] = [
    "#####    #####",
    "#...#    #...#",
    "#...#    #...#",
    "#####    #####",
    "#.###    #####",
    "#.###    #####",
    "#.###    #####",
    "#.###    #####",
    "#.####  ######",
    " #.##########",
    "  #.#########",
    "   ########",
]
P["i_mult"] = [
    " ######       ",
    "########      ",
    "##    ##      ",
    "      ##      ",
    "     ###      ",
    "    ###  ## ##",
    "   ###    ### ",
    "  ###     ### ",
    " ###     ## ##",
    "########      ",
    "########      ",
]
P["i_board"] = [
    "   ############   ",
    " ##............## ",
    "#..####....####..#",
    "#................#",
    " ##............## ",
    "   ############   ",
    "  ##   ####   ##  ",
    "    ##      ##    ",
]


def bubble_icon(name, icon, dx=0, dy=0):
    rows = P[icon]
    w, h = max(len(r) for r in rows), len(rows)
    x = (24 - w) // 2 + dx
    y = (24 - h) // 2 + dy
    frame(name, 24, 24, 12, 23, [("bubble", 0, 0), (icon, x, y)])


bubble_icon("pu_jetpack", "i_jetpack")
bubble_icon("pu_sneakers", "i_sneakers", dx=0, dy=0)
bubble_icon("pu_magnet", "i_magnet")
bubble_icon("pu_mult", "i_mult", dx=1)
bubble_icon("pu_board", "i_board")

# ---------------------------------------------------------------- HUD
DIGITS = {
    "0": [" ###### ", "########", "###  ###", "##    ##", "##    ##", "##    ##",
          "##    ##", "##    ##", "###  ###", "########", " ###### "],
    "1": ["   ##   ", "  ###   ", " ####   ", "## ##   ", "   ##   ", "   ##   ",
          "   ##   ", "   ##   ", "   ##   ", "########", "########"],
    "2": [" ###### ", "########", "##    ##", "      ##", "     ###", "   #### ",
          "  ###   ", " ###    ", "###     ", "########", "########"],
    "3": [" ###### ", "########", "##    ##", "      ##", "   #### ", "   #### ",
          "      ##", "      ##", "##    ##", "########", " ###### "],
    "4": ["    ### ", "   #### ", "  ## ## ", " ##  ## ", "##   ## ", "########",
          "########", "     ## ", "     ## ", "     ## ", "     ## "],
    "5": ["########", "########", "##      ", "##      ", "####### ", "########",
          "      ##", "      ##", "##    ##", "########", " ###### "],
    "6": [" ###### ", "########", "##      ", "##      ", "####### ", "########",
          "##    ##", "##    ##", "##    ##", "########", " ###### "],
    "7": ["########", "########", "      ##", "     ## ", "    ##  ", "   ##   ",
          "   ##   ", "  ##    ", "  ##    ", "  ##    ", "  ##    "],
    "8": [" ###### ", "########", "##    ##", "##    ##", " ###### ", " ###### ",
          "##    ##", "##    ##", "##    ##", "########", " ###### "],
    "9": [" ###### ", "########", "##    ##", "##    ##", "##    ##", "########",
          " #######", "      ##", "      ##", "########", " ###### "],
}
for d, rows in DIGITS.items():
    P["font_" + d] = rows
    frame("font_" + d, 8, 11, 0, 0, [("font_" + d, 0, 0)])
    FRAMES["font_" + d] += ("notrim",)

P["hud_coin"] = [
    "   ####   ",
    " ##....## ",
    " #......# ",
    "#...##...#",
    "#...##...#",
    "#...##...#",
    "#...##...#",
    " #......# ",
    " ##....## ",
    "   ####   ",
]
P["hud_board"] = [
    "  ##########  ",
    " #..........# ",
    "#..########..#",
    "#............#",
    " #..........# ",
    "  ##########  ",
    "   ##  ##  ## ",
    " ##  ##  ##   ",
]
for n in ("hud_coin", "hud_board"):
    frame(n, 16, 16, 0, 0, [(n, 0, 0)])
    FRAMES[n] += ("notrim",)



# ---------------------------------------------------------------- title logo (procedural)
LOGO_GLYPHS = {
    "R": ["#####.", "######", "##..##", "##..##", "#####.", "####..", "##.##.", "##..##", "##..##"],
    "A": ["..##..", ".####.", "######", "##..##", "##..##", "######", "######", "##..##", "##..##"],
    "I": ["##", "##", "##", "##", "##", "##", "##", "##", "##"],
    "L": ["##....", "##....", "##....", "##....", "##....", "##....", "##....", "######", "######"],
    "U": ["##..##", "##..##", "##..##", "##..##", "##..##", "##..##", "##..##", "######", ".####."],
    "N": ["##..##", "###.##", "###.##", "######", "######", "##.###", "##.###", "##..##", "##..##"],
    "E": ["######", "######", "##....", "##....", "#####.", "#####.", "##....", "######", "######"],
}


def _glyph_mask(ch, cell=4):
    import numpy as np
    g = LOGO_GLYPHS[ch]
    gh, gw = len(g), len(g[0])
    m = np.zeros((gh * cell, gw * cell), bool)
    filled = lambda r, c: 0 <= r < gh and 0 <= c < gw and g[r][c] == "#"
    for r in range(gh):
        for c in range(gw):
            if not filled(r, c):
                continue
            m[r * cell:(r + 1) * cell, c * cell:(c + 1) * cell] = True
            # round convex corners with a small triangle cut
            for dr, dc in ((-1, -1), (-1, 1), (1, -1), (1, 1)):
                if not filled(r + dr, c) and not filled(r, c + dc):
                    y0 = r * cell if dr < 0 else (r + 1) * cell - 1
                    x0 = c * cell if dc < 0 else (c + 1) * cell - 1
                    for k in range(2):
                        for j in range(2 - k):
                            m[y0 - dr * k, x0 - dc * j] = False
    return m


def make_logo(text="RAIL RUNNER", cell=4, gap=1, space=12, shear=0.2,
              bounce=(0, -3, 1, -2, 0, 2, -2, 0, -3, 1, -1),
              drips=((1, 9, 10), (6, 2, 14), (9, 17, 9)), depth=4, outline=2):
    import numpy as np
    H, W = 90, 340
    top, left = 14, 6
    faces = []                       # (mask on canvas, letter top y)
    x = left
    for i, ch in enumerate(text):
        if ch == " ":
            x += space
            continue
        g = _glyph_mask(ch, cell)
        m = np.zeros((H, W), bool)
        y = top + bounce[i % len(bounce)]
        m[y:y + g.shape[0], x:x + g.shape[1]] = g
        faces.append([m, y, g.shape[0], x, g.shape[1]])
        x += g.shape[1] + gap + 2 * outline
    # drips: (letter index, x offset in letter, length)
    for li, dx, ln in drips:
        m, y, gh, gx, gw = faces[li]
        cx = gx + dx
        yb = y + gh
        m[yb - 2:yb + ln, cx:cx + 4] = True
        for yy in range(-3, 4):
            for xx in range(-3, 4):
                if (xx - 0.5) ** 2 + yy * yy <= 9:
                    m[yb + ln + yy, cx + 2 + xx] = True

    def shear_m(m):
        out = np.zeros_like(m)
        for yy in range(H):
            d = int(round((H - yy) * shear)) - int(round((H - top - 40) * shear))
            if d >= 0:
                out[yy, d:] = m[yy, :W - d]
            else:
                out[yy, :W + d] = m[yy, -d:]
        return out

    for f in faces:
        f[0] = shear_m(f[0])

    def dilate(m, r):
        out = m.copy()
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                if dx * dx + dy * dy <= r * r + 1:
                    out |= np.roll(np.roll(m, dy, 0), dx, 1)
        return out

    ink = np.full((H, W), " ", dtype="<U1")
    allf = np.zeros((H, W), bool)
    for f in faces:
        allf |= f[0]
    body = dilate(allf, outline)
    ext = body.copy()
    for k in range(1, depth + 1):
        ext |= np.roll(np.roll(body, k, 0), k, 1)
    ink[ext] = "#"
    for m, y, gh, gx, gw in faces:
        ink[dilate(m, outline)] = "#"
        ys, xs = np.nonzero(m)
        for yy, xx in zip(ys, xs):
            rel = (yy - y) / gh
            if rel > 0.86 and rel <= 1.0:
                ink[yy, xx] = "+"
            elif rel > 0.66 and rel <= 1.0:
                ink[yy, xx] = ":"
            else:
                ink[yy, xx] = "."
    rows = ["".join(r).rstrip() for r in ink]
    while rows and not rows[0]:
        rows.pop(0)
    while rows and not rows[-1]:
        rows.pop()
    lft = min(len(r) - len(r.lstrip()) for r in rows if r)
    return [r[lft:] for r in rows]


P["logo"] = make_logo()
SPARK = [
    "    #    ",
    "    #    ",
    "   #.#   ",
    "  #...#  ",
    "##.....##",
    "  #...#  ",
    "   #.#   ",
    "    #    ",
    "    #    ",
]
P["logo_spark"] = SPARK
P["logo_dot"] = ["##", "##"]
frame("title_logo", 320, 90, 0, 0, [("logo", 2, 4), ("logo_spark", 6, 2), ("logo_spark", 286, 0),
                                   ("logo_dot", 2, 12), ("logo_dot", 298, 12)])
FRAMES["title_logo"] += ("origin",)



# ---------------------------------------------------------------- title screen Rex (front view)
P["tr_face"] = [
    "                 ########",
    "              ##############",
    "            ##################",
    "           ####################",
    "          ######################",
    " ####### ########################",
    "#.......#########.....###########",
    " ##......#######.#####.###########",
    "   ####.#######.#######.#########",
    "       ########.#######.#########",
    "       ##################.#######",
    "       ####...............#######",
    "      ####.................####",
    "     #.##..####.............##.#",
    "     #..#.#....##.....######.#..#",
    "     #..#.................#..#..#",
    "     #..#...####.....######..#..#",
    "     #.##..#....#...#......#.##.#",
    "      ##...#..##.#..#..##..#.##",
    "       #...#..##.#..#..##.#..#",
    "       #....####....#####...#",
    "       #..........#.........#",
    "       #.........##.........#",
    "       #....................#",
    "       #...#..........###...#",
    "        #...###########.#..#",
    "        #....#.........#..#",
    "         #....#########..#",
    "          ##.............#",
    "            ###.......###",
    "               #######",
]
P["tr_neck"] = [
    "#........#",
    "#........#",
    "#........#",
    "#........#",
    "#........#",
]
P["tr_tee"] = sym([
    "   ######     ",
    "  #......#    ",
    " #........#   ",
    "#..........###",
    "#.............",
    "#............#",
    "#............#",
    "#...........##",
    "#........#####",
    "#.........####",
    "#..........###",
    "#..........###",
    "#.........###.",
    "#.........##..",
    "#.............",
    "#.............",
    "#.............",
    "#.............",
    "#.............",
    " #############",
])
P["tr_shorts"] = sym([
    "#############",
    "#.###########",
    "#.###########",
    "#.###########",
    "#.###########",
    "#.#######    ",
    "#.#######    ",
    "#########    ",
])
P["tr_leg"] = ["#....#"] * 9
P["tr_shoe"] = [
    "   ######   ",
    "  #......#  ",
    " #..#..#..# ",
    " #...##...# ",
    "#..........#",
    "#..........#",
    "############",
    "#..........#",
    " ########## ",
]
P["tr_can"] = [
    "  ##  ",
    " #..# ",
    "#....#",
    "#....#",
    "######",
    "#.####",
    "#.####",
    "#.####",
    "#.####",
    "#.####",
    "######",
]
P["tr_arm_up"] = [
    " ######",
    "#......#",
    "#.#.#..#",
    "#.#.#..#",
    "#......#",
    " ######",
    " #######",
    " #.....#",
    " #.....#",
    " #.....#",
    " #.....#",
    " #.....#",
    " #......#",
    " #.......#",
    "  #.......#",
    "   #.......#",
    "    #.......#",
    "     #.......#",
    "      #.......#",
    "       #.......#",
    "        #.......#",
    "         #.......#",
]
P["tr_arm_hip"] = [
    "#......#",
    " #......#",
    "  #......#",
    "   #......#",
    "    #......#",
    "     #......#",
    "      #.....#",
    "      #.....#",
    "     #.....#",
    "    #.....#",
    "   #.....#",
    "  ########",
    " #.......#",
    " #.#.#...#",
    "  #######",
]
P["tr_mist"] = [
    "        #  # ",
    "     #     ##",
    "   #  # #  # ",
    " #  ## # #   ",
    "#  #  #  #   ",
    "###  ##      ",
    "##  #        ",
]
frame("title_rex", 100, 120, 32 + 20, 82 + 20, [(o[0], o[1] + 20, o[2] + 20) for o in [
    ("tr_leg", 22, 65), ("tr_leg", 36, 65),
    ("tr_shoe", 19, 74), ("tr_shoe", 35, 74),
    ("tr_shorts", 19, 57),
    ("tr_neck", 27, 33),
    ("tr_tee", 18, 37),
    ("tr_arm_hip", 42, 38),
    ("tr_arm_up", 3, 16),
    ("tr_can", 4, 6),
    ("tr_mist", 7, 0),
    ("tr_face", 14, 6),
]])


# ---------------------------------------------------------------- output
def main():
    out = [
        "; Rail Runner sprite masters. GENERATED by art/compose.py from its part library:",
        "; edit compose.py and re-run it rather than editing this file by hand.",
        "; Format: see tools/sprites.py. Anchor (ax, ay) = ground point under the body centre.",
        "",
    ]
    for n in ORDER:
        rows, ax, ay = render(n)
        out.append(f"@ {n} {ax} {ay}")
        out += rows
        out.append("")
    open(os.path.join(HERE, "sprites.txt"), "w").write("\n".join(out) + "\n")
    print(len(ORDER), "sprites")


if __name__ == "__main__":
    main()

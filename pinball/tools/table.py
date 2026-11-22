#!/usr/bin/env python3
"""Neural Pinball table: vector geometry, ids and lamp layout (shared by art.py and gen.py).

All coordinates are table pixels: x right, y down, origin at the table's top-left; the table is
256 x 322 px (the full height under the menu bar). Physics shapes are listed in SHAPES as dicts;
gen.py turns them into C (core/phys.h ShapeDef) and art.py draws the playfield from the same
numbers, so the picture and the physics can't drift apart.
"""
import math

W, H = 256, 322
BALL_R = 4.0

# ---------------------------------------------------------------------------------- materials
# name: (e, vrest px/s, mu, kick px/s, kmin px/s): impacts slower than vrest don't bounce; a
# kicker adds `kick` along the normal when hit at kmin or more (gen.py converts to per step)
STEPS_PER_SEC = 60.15 * 2          # physics steps per second (core/phys.h STEPS_PER_TICK = 2)
MATERIALS = [
    ("WALL",    0.45, 48, 0.08, 0, 0),
    ("RUBBER",  0.75, 48, 0.12, 0, 0),
    ("FLIPPER", 0.55, 58, 0.02, 0, 0),    # low friction: the real ball rolls along it
    ("SLING",   0.70, 48, 0.12, 746, 144),
    ("BUMPER",  0.60, 48, 0.08, 986, 0),
    ("DROP",    0.30, 34, 0.12, 0, 0),
    ("RAIL",    0.35, 48, 0.03, 0, 0),
]
MAT = {m[0]: i for i, m in enumerate(MATERIALS)}

# ---------------------------------------------------------------------------------- ids
IDS = """
LANE0 LANE1 LANE2 LANE3 LANE4
BUMPER0 BUMPER1 BUMPER2
SLING_L SLING_R
DROP0 DROP1 DROP2
SPINNER ORBIT RAMP_MADE SINK
INLANE_L INLANE_R OUTLANE_L OUTLANE_R KICKBACK
SHOOTER_EXIT POST
""".split()
ID = {n: i + 1 for i, n in enumerate(IDS)}

# shape flags (core/phys.h)
SF = dict(ONEWAY=0x0001, INSIDE=0x0002, KICK=0x0004, BUMPER=0x0008, DROP=0x0010, EVENT=0x0020,
          OFF=0x0040, LAYER=0x0080, SINK=0x0100, BOWL=0x0200, KICKER=0x0400, ARMED=0x0800,
          SPINNER=0x1000, DRAIN=0x2000)
KIND = dict(SEG=1, CIRCLE=2, ARC=3, LINE=4, DISC=5)

SHAPES = []


def seg(a, b, mat="WALL", flags=(), layers=1, id=0, rad=0.0):
    SHAPES.append(dict(kind="SEG", a=a, b=b, mat=mat, flags=flags, layers=layers, id=id, rad=rad))


def poly(points, mat="WALL", layers=1, rad=0.0, closed=False, flags=()):
    pts = list(points) + ([points[0]] if closed else [])
    for a, b in zip(pts, pts[1:]):
        seg(a, b, mat, flags, layers, rad=rad)


def circle(c, r, mat="RUBBER", flags=(), layers=1, id=0):
    SHAPES.append(dict(kind="CIRCLE", c=c, r=r, mat=mat, flags=flags, layers=layers, id=id))


def arc(c, r, a0, a1, mat="WALL", layers=1):
    """circular wall from angle a0 to a1 (degrees, y down: 90 = down, 270 = up), a1 > a0, <= 180"""
    while a1 - a0 > 180:                       # split long arcs
        arc(c, r, a0, a0 + 90, mat, layers)
        a0 += 90
    SHAPES.append(dict(kind="ARC", c=c, r=r, a0=a0, a1=a1, mat=mat, layers=layers, flags=(), id=0))


def line(a, b, id, flags=(), layers=1, kick=None):
    """sensor line; kick = (angle deg, speed px/step) for kickers"""
    SHAPES.append(dict(kind="LINE", a=a, b=b, id=id, flags=flags, layers=layers, mat="WALL", kick=kick))


def disc(c, r, id=0, flags=(), layers=1):
    SHAPES.append(dict(kind="DISC", c=c, r=r, id=id, flags=flags, layers=layers, mat="WALL"))


def layer_line(a, b, to_pos, to_neg):
    """crossing towards +n (n = (-uy, ux) of a->b) sets layer to_pos, towards -n to_neg"""
    line(a, b, (to_neg << 8) | to_pos, flags=("LAYER",), layers=3)


def tangent_point(p, c, r, upper=True):
    """point where a line from p touches the circle (c, r); the upper one by default"""
    d = math.hypot(p[0] - c[0], p[1] - c[1])
    th = math.atan2(p[1] - c[1], p[0] - c[0])
    al = math.acos(r / d)
    cands = [(c[0] + r * math.cos(th + s * al), c[1] + r * math.sin(th + s * al)) for s in (1, -1)]
    return min(cands, key=lambda q: q[1]) if upper else max(cands, key=lambda q: q[1])


def mirror(p):
    return (CX * 2 - p[0], p[1])


# ---------------------------------------------------------------------------------- layout
CX = 121.0                       # playfield centre line (between x = 4 and the shooter lane, 238)

# flippers
FLIP_LEN, FLIP_RB, FLIP_RT = 40.0, 4.5, 2.5
FLIP_L = (77.0, 286.0)
FLIP_R = mirror(FLIP_L)
FLIP_REST, FLIP_UP = 30.0, -28.0  # degrees below/above horizontal (left flipper)

# outer walls
LEFT_X, RIGHT_X, TOP_Y = 4.0, 252.0, 4.0
ARC_R = 60.0
SHOOT_X0, SHOOT_X1 = 238.0, 252.0          # shooter lane
SHOOT_TOP = 116.0                           # top of the inner shooter wall
PLUNGER_Y = 302.0                           # plunger tip at rest
PLUNGER_PULL = 16.0

# The layout follows the flippers' measured shot cones (tests/aim.c): from a cradle the left
# flipper shoots between about 53 deg (tip) and 100 deg (middle), the right one between 80 and
# 127 deg. The centre column (bowl, drop targets) is the easy shot, the ramp (left flipper,
# about 70 deg) and the orbit (right flipper, about 115 deg, helped by a funnel) need late flips;
# the pop bumpers sit out of the main lines, fed by the top lanes.

# top lanes (T R A I N), upper left, feeding the bumpers
LANE_X = [30.0, 50.0, 70.0, 90.0, 110.0, 130.0]     # guide positions
LANE_TOP, LANE_BOT = 32.0, 54.0
LANE_SENSOR_Y = 44.0

BUMPERS = [(52.0, 84.0), (98.0, 80.0), (75.0, 112.0)]
BUMPER_R = 11.0

# left orbit lane with the Attention spinner
ORBIT_X = 21.0                              # inner wall of the left orbit lane
ORBIT_TOP, ORBIT_BOT = 92.0, 134.0
SPINNER_Y = 124.0

# the orbit funnel: from the lane's inner wall down-right, deflects ~115 deg shots into the lane
FUNNEL_END = (60.0, 172.0)

# gradient-descent bowl and its sink
BOWL_C, BOWL_R, SINK_R = (121.0, 172.0), 21.0, 5.0

# dropout targets: a bank of 3 above the bowl, facing the flippers
DROP_C, DROP_W, DROP_GAP = (121.0, 120.0), 8.0, 2.0
DROP_FACE = (121.0, 300.0)

# backprop ramp (layer 2): entrance lane x 146..170 going up, U-turn, return lane x 206..222
RAMP_X = (146.0, 170.0, 206.0, 222.0)
RAMP_TOP_Y = 98.0
RAMP_TOP_C = ((RAMP_X[0] + RAMP_X[3]) / 2, RAMP_TOP_Y)
RAMP_ENTRY_Y = 150.0
RAMP_EXIT_Y = 214.0

# slingshots and lanes at the bottom
SLING_L = [(34.0, 214.0), (34.0, 244.0), (59.0, 258.0)]    # top, bottom-left, bottom-right
INLANE_X = 20.0                              # inlane/outlane separator (left)
INLANE_TOP, INLANE_BEND = 228.0, 257.0


def build():
    SHAPES.clear()
    # ---- outer boundary
    seg((LEFT_X, H + 10), (LEFT_X, TOP_Y + ARC_R))
    arc((LEFT_X + ARC_R, TOP_Y + ARC_R), ARC_R, 180, 270)
    seg((LEFT_X + ARC_R, TOP_Y), (RIGHT_X - ARC_R, TOP_Y))
    arc((RIGHT_X - ARC_R, TOP_Y + ARC_R), ARC_R, 270, 360)
    seg((RIGHT_X, TOP_Y + ARC_R), (RIGHT_X, H + 10))
    # ---- shooter lane: inner wall, one-way gate at the top (passes balls going up), deflector
    seg((SHOOT_X0, H + 10), (SHOOT_X0, SHOOT_TOP), rad=1.0)
    seg((RIGHT_X, SHOOT_TOP - 14), (SHOOT_X0 + 1, SHOOT_TOP - 5), mat="RAIL", flags=("ONEWAY",))
    seg((SHOOT_X0, SHOOT_TOP), (215.0, SHOOT_TOP + 18), rad=1.0)          # to the right inlane
    line((SHOOT_X0, SHOOT_TOP - 20), (RIGHT_X, SHOOT_TOP - 20), ID["SHOOTER_EXIT"])
    # ---- top lanes: guides with round tops; rollover sensors
    for i, x in enumerate(LANE_X):
        seg((x, LANE_TOP), (x, LANE_BOT), mat="RUBBER", rad=1.5)
    for i in range(5):
        line((LANE_X[i] + 2, LANE_SENSOR_Y), (LANE_X[i + 1] - 2, LANE_SENSOR_Y), ID["LANE%d" % i])
    # ---- pop bumpers ("neurons")
    for i, c in enumerate(BUMPERS):
        circle(c, BUMPER_R, mat="BUMPER", flags=("BUMPER",), id=ID["BUMPER%d" % i])
    # ---- left orbit lane (gradient lane) with the spinner
    seg((ORBIT_X, ORBIT_TOP), (ORBIT_X, ORBIT_BOT), rad=1.5)
    # one-way gate at the top of the orbit lane: balls leave upwards; balls coming over the top
    # from the right bounce off it onto the playfield instead of dropping down the lane
    seg((ORBIT_X + 7, ORBIT_TOP - 4), (LEFT_X, ORBIT_TOP - 12), mat="RAIL", flags=("ONEWAY",))
    seg((ORBIT_X, ORBIT_BOT), FUNNEL_END, rad=1.5)
    line((LEFT_X, SPINNER_Y), (ORBIT_X - 1.5, SPINNER_Y), ID["SPINNER"], flags=("SPINNER",))
    line((LEFT_X + 20, 30), (LEFT_X + 30, 12), ID["ORBIT"])     # across the top-left arc
    # ---- bowl and sink
    disc(BOWL_C, BOWL_R, flags=("BOWL",))
    disc(BOWL_C, SINK_R, id=ID["SINK"], flags=("SINK",))
    # ---- dropout targets
    nx, ny = DROP_FACE[0] - DROP_C[0], DROP_FACE[1] - DROP_C[1]
    nl = math.hypot(nx, ny)
    nx, ny = nx / nl, ny / nl                       # facing the flippers
    tx, ty = -ny, nx                                # along the bank
    total = 3 * DROP_W + 2 * DROP_GAP
    for i in range(3):
        o = -total / 2 + i * (DROP_W + DROP_GAP)
        a = (DROP_C[0] + tx * o, DROP_C[1] + ty * o)
        b = (DROP_C[0] + tx * (o + DROP_W), DROP_C[1] + ty * (o + DROP_W))
        seg(a, b, mat="DROP", flags=("DROP",), id=ID["DROP%d" % i], rad=1.5)
    back = 2.5                      # backstop just behind the bank: a dropped target leaves no pocket
    ba = (DROP_C[0] - tx * (total / 2 + 1) - nx * back, DROP_C[1] - ty * (total / 2 + 1) - ny * back)
    bb = (DROP_C[0] + tx * (total / 2 + 1) - nx * back, DROP_C[1] + ty * (total / 2 + 1) - ny * back)
    seg(ba, bb, rad=1.0)
    # a peaked roof over the bank, so balls coming down from above roll off instead of resting
    peak = (DROP_C[0] - nx * (back + 9), DROP_C[1] - ny * (back + 9))
    seg(ba, peak, rad=1.0)
    seg(peak, bb, rad=1.0)
    # ---- backprop ramp (layer 2)
    x0, x1, x2, x3 = RAMP_X
    rc = RAMP_TOP_C
    ic = ((x1 + x2) / 2, RAMP_TOP_Y)
    ro, ri = (x3 - x0) / 2, (x2 - x1) / 2
    # the mouth: the first 14 px of the entrance rails are solid on the playfield too, so a ball
    # can only enter head-on (the layer sensor sits between them, 6 px up from their ends)
    mouth = RAMP_ENTRY_Y + 6
    seg((x0, mouth), (x0, mouth - 14), mat="RAIL", layers=3, rad=1.0)
    seg((x1, mouth), (x1, mouth - 14), mat="RAIL", layers=3, rad=1.0)
    seg((x0, mouth - 14), (x0, rc[1]), mat="RAIL", layers=2, rad=1.0)
    seg((x1, mouth - 14), (x1, rc[1]), mat="RAIL", layers=2, rad=1.0)
    seg((x2, RAMP_EXIT_Y), (x2, rc[1]), mat="RAIL", layers=2, rad=1.0)
    seg((x3, RAMP_EXIT_Y), (x3, rc[1]), mat="RAIL", layers=2, rad=1.0)
    arc(rc, ro, 180, 360, mat="RAIL", layers=2)
    arc(ic, ri, 180, 360, mat="RAIL", layers=2)
    # n = (0, 1) for these left-to-right lines: +n is down the table
    layer_line((x0 + 1, RAMP_ENTRY_Y), (x1 - 1, RAMP_ENTRY_Y), 1, 2)   # up into the ramp -> 2, down -> 1
    layer_line((x2, RAMP_EXIT_Y - 4), (x3, RAMP_EXIT_Y - 4), 1, 0)   # leaving the return lane -> 1
    line((ic[0], ic[1] - ri - 1), (ic[0], rc[1] - ro + 1), ID["RAMP_MADE"], layers=2)
    # ---- slingshots: kicking face top -> bottom-right, passive other sides
    for side in (0, 1):
        t, bl, br = SLING_L if side == 0 else [mirror(p) for p in SLING_L]
        sid = ID["SLING_L"] if side == 0 else ID["SLING_R"]
        if side == 0:
            seg(t, br, mat="SLING", flags=("KICK",), id=sid, rad=1.5)
        else:
            seg(br, t, mat="SLING", flags=("KICK",), id=sid, rad=1.5)
        seg(t, bl, mat="RUBBER", rad=1.5)
        seg(bl, br, mat="RUBBER", rad=1.5)
    # ---- inlane/outlane guides, running tangent into the flipper bases
    for side in (0, 1):
        fx = FLIP_L if side == 0 else FLIP_R
        gx = INLANE_X if side == 0 else 2 * CX - INLANE_X
        top, bend = (gx, INLANE_TOP), (gx, INLANE_BEND)
        tp = tangent_point(bend, fx, FLIP_RB + 1.0)
        seg(top, bend, rad=1.0)
        seg(bend, tp, rad=1.0)
        circle(top, 2.5, mat="RUBBER", id=ID["POST"])
        # sensors: inlane (between sling and guide) and outlane (outside the guide)
        if side == 0:
            line((gx + 2, INLANE_TOP + 8), (SLING_L[0][0] - 2, INLANE_TOP + 8), ID["INLANE_L"])
            line((LEFT_X, INLANE_TOP + 8), (gx - 1.5, INLANE_TOP + 8), ID["OUTLANE_L"])
        else:
            line((2 * CX - SLING_L[0][0] + 2, INLANE_TOP + 8), (gx - 2, INLANE_TOP + 8), ID["INLANE_R"])
            line((gx + 1.5, INLANE_TOP + 8), (SHOOT_X0 - 1, INLANE_TOP + 8), ID["OUTLANE_R"])
    # kickback at the bottom of the left outlane
    line((LEFT_X, 280), (LEFT_X + 30, 280), ID["KICKBACK"], flags=("KICKER",), kick=(-86, 1588))   # px/s
    # apron: below the flippers the guides end; nothing else (the drain is open)
    return SHAPES


def ramp_box():
    """layer 2 exists only inside this box (px): physics puts stray ramp balls back on layer 1"""
    return (RAMP_X[0] - 6, RAMP_TOP_Y - (RAMP_X[3] - RAMP_X[0]) / 2 - 6, RAMP_X[3] + 6, RAMP_EXIT_Y + 4)


def flippers():
    """(pivot, rest angle, up angle) per flipper, angles of pivot->tip in degrees (y down)"""
    return [
        dict(pivot=FLIP_L, rest=FLIP_REST, up=FLIP_UP),
        dict(pivot=FLIP_R, rest=180 - FLIP_REST, up=180 - FLIP_UP),
    ]


# ---------------------------------------------------------------------------------- checks
def _dist_point_seg(p, a, b):
    ax, ay, bx, by = a[0], a[1], b[0], b[1]
    dx, dy = bx - ax, by - ay
    l2 = dx * dx + dy * dy
    t = 0 if l2 == 0 else max(0, min(1, ((p[0] - ax) * dx + (p[1] - ay) * dy) / l2))
    qx, qy = ax + t * dx, ay + t * dy
    return math.hypot(p[0] - qx, p[1] - qy)


def clearance(p, layer=1):
    """free distance from point p to the nearest solid shape on a layer (minus thickness)"""
    best = 1e9
    for s in SHAPES:
        if not (s["layers"] & layer) or s["kind"] in ("LINE", "DISC"):
            continue
        if s["kind"] == "SEG":
            d = _dist_point_seg(p, s["a"], s["b"]) - s["rad"]
        elif s["kind"] == "CIRCLE":
            d = math.hypot(p[0] - s["c"][0], p[1] - s["c"][1]) - s["r"]
        else:
            dx, dy = p[0] - s["c"][0], p[1] - s["c"][1]
            ang = math.degrees(math.atan2(dy, dx)) % 360
            a0, a1 = s["a0"] % 360, s["a1"] % 360 if s["a1"] % 360 else 360
            if a0 <= ang <= a1 or (s["a1"] >= 360 and ang <= s["a1"] - 360):
                d = abs(math.hypot(dx, dy) - s["r"])
            else:
                d = min(math.hypot(p[0] - (s["c"][0] + s["r"] * math.cos(math.radians(a))),
                                   p[1] - (s["c"][1] + s["r"] * math.sin(math.radians(a)))) for a in (s["a0"], s["a1"]))
        best = min(best, d)
    return best


if __name__ == "__main__":
    build()
    print(len(SHAPES), "shapes")
    # lanes the ball must fit through: (name, point, layer)
    probes = [
        ("shooter lane", ((SHOOT_X0 + SHOOT_X1) / 2, 200), 1),
        ("left orbit lane", ((LEFT_X + ORBIT_X) / 2, 120), 1),
        ("bumper gap", (30, 100), 1),
        ("orbit mouth", (14, 140), 1),
        ("left inlane", (27, 238), 1),
        ("right inlane", (2 * CX - 27, 238), 1),
        ("left outlane", (12, 238), 1),
        ("right outlane", (2 * CX - 12, 238), 1),
        ("ramp up lane", ((RAMP_X[0] + RAMP_X[1]) / 2, 150), 2),
        ("ramp return", ((RAMP_X[2] + RAMP_X[3]) / 2, 150), 2),
        ("ramp top", (RAMP_TOP_C[0], RAMP_TOP_Y - 12), 2),
        ("lane T", ((LANE_X[0] + LANE_X[1]) / 2, 44), 1),
    ]
    # the inlanes: walk the middle of the channel from the top to the flipper
    for side in (0, 1):
        m = (lambda p: p) if side == 0 else mirror
        path = [(27, 232), (27, 246), (30, 252), (38, 256), (48, 260), (58, 265), (66, 270)]
        for i, p in enumerate(path):
            probes.append(("%s inlane %d" % ("left" if side == 0 else "right", i), m(p), 1))
    bad = 0
    for name, p, layer in probes:
        # the best clearance within 3 px of the probe: is there a passage here?
        c, p = max((clearance((p[0] + dx * 0.5, p[1] + dy * 0.5), layer), (p[0] + dx * 0.5, p[1] + dy * 0.5))
                   for dx in range(-6, 7) for dy in range(-6, 7))
        ok = c > BALL_R + 0.3
        bad += not ok
        print(f"  {name:16s} at ({p[0]:.0f},{p[1]:.0f}): clearance {c:4.1f} px {'OK' if ok else 'TOO TIGHT'}")
    print("clearance:", "all OK" if not bad else "%d TOO TIGHT" % bad)

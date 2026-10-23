"""Rail Runner reference simulation (integer-exact; the 68000 game mirrors it).

World objects are kept in absolute z (zu). The player's z advances by speed_at(z) every tick, so
the world the player sees at tick t does not depend on the player's actions. That makes an exact
breadth-first search over the player's states possible (test_escape.py).

Per tick, in this order (the game does the same):
  1. z += v; oncoming trains move ONCOMING_V toward the player.
  2. env: for each lane, ground height and "high bar present" over [z - PZ, z + PZ].
  3. player_step(): input, lateral move, vertical physics, collisions.
"""
import os
from dataclasses import dataclass, field

from params import *  # noqa: F401,F403

HERE = os.path.dirname(os.path.abspath(__file__))

# object kinds
TRAIN, RAMP, LOW, HIGH, WALL, ONCOMING = range(6)
KIND_NAMES = ["train", "ramp", "low", "high", "wall", "oncoming"]

# inputs
NONE, LEFT, RIGHT, JUMP, ROLL = range(5)

# events returned by player_step
OK, CRASH, STUMBLE = 0, 1, 2

TIER_SPEED = [V0, 150, 170, 195]


# ------------------------------------------------------------------ patterns
@dataclass
class Pattern:
    name: str
    tier: int
    weight: int
    rows: int
    objs: list            # (kind, lane, row0, row1): rows [row0, row1), row 0 = nearest
    coins: list = field(default_factory=list)   # (row*4+quarter, lane, y) filled by coins.py


def load_patterns(path=os.path.join(HERE, "patterns.txt")):
    pats, cur, grid = [], None, []

    def finish():
        if cur is None:
            return
        name, tier, weight = cur
        g = grid[::-1]                         # g[0] = nearest row
        objs = []
        for lane in range(3):
            col = [r[lane] for r in g]
            r = 0
            while r < len(col):
                c = col[r]
                if c in "T/o":
                    e = r
                    while e < len(col) and col[e] == c:
                        e += 1
                    if c == "T":
                        objs.append((TRAIN, lane, r, e))
                    elif c == "/":
                        if e >= len(col) or col[e] != "T":
                            raise SystemExit(f"{name}: ramp in lane {lane} not followed by a train")
                        objs.append((RAMP, lane, r, e))
                    else:
                        raise SystemExit(f"{name}: 'o' without 'O' below it in lane {lane}")
                    r = e
                    continue
                if c == "O":
                    e = r + 1
                    while e < len(col) and col[e] == "o":
                        e += 1
                    objs.append((ONCOMING, lane, r, e))
                    r = e
                    continue
                kind = {"_": LOW, "^": HIGH, "#": WALL}.get(c)
                if kind is not None:
                    objs.append((kind, lane, r, r + 1))
                elif c != ".":
                    raise SystemExit(f"{name}: unknown glyph {c!r}")
                r += 1
        pats.append(Pattern(name, tier, weight, len(g), objs))

    for line in open(path):
        line = line.split(";", 1)[0].rstrip()
        if not line.strip():
            continue
        if line.startswith("="):
            finish()
            _, name, tier, weight = line.split()
            cur, grid = (name, int(tier), int(weight)), []
        else:
            cells = line.split()
            if len(cells) != 3:
                raise SystemExit(f"bad row {line!r}")
            grid.append(cells)
    finish()
    return pats


# ------------------------------------------------------------------ world
@dataclass
class Obj:
    kind: int
    lane: int
    z0: int               # near end (for oncoming trains: the front, which moves)
    z1: int               # far end
    pat: int = -1


def place(pat, zs, player_z, v):
    """World objects for pattern `pat` whose row 0 starts at zs, spawned when the player is at
    player_z moving at speed v (oncoming trains are timed to meet the player at their 'O' row)."""
    out = []
    for kind, lane, r0, r1 in pat.objs:
        a, b = zs + r0 * U, zs + r1 * U
        if kind == LOW:
            a += (U - LOW_D) // 2; b = a + LOW_D
        elif kind == HIGH:
            a += (U - HIGH_D) // 2; b = a + HIGH_D
        elif kind == WALL:
            a += (U - WALL_D) // 2; b = a + WALL_D
        elif kind == ONCOMING:
            t_meet = (a - player_z) // v
            length = b - a
            a = a + ONCOMING_V * t_meet
            b = a + length
        out.append(Obj(kind, lane, a, b))
    return out


def move_world(objs):
    for o in objs:
        if o.kind == ONCOMING:
            o.z0 -= ONCOMING_V
            o.z1 -= ONCOMING_V


def env_at(objs, z):
    """(ground[3], bar[3]) over the player's depth range [z - PZ, z + PZ]."""
    lo, hi = z - PZ, z + PZ
    g = [0, 0, 0]
    bar = [False, False, False]
    for o in objs:
        if o.z1 <= lo or o.z0 >= hi:
            continue
        k, ln = o.kind, o.lane
        if k == HIGH:
            bar[ln] = True
            continue
        if k == RAMP:
            top = hi if hi < o.z1 else o.z1
            h = TRAIN_H * (top - o.z0) // (o.z1 - o.z0)
        elif k == LOW:
            h = LOW_H
        else:
            h = TRAIN_H
        if h > g[ln]:
            g[ln] = h
    return g, bar


# ------------------------------------------------------------------ player
@dataclass
class Player:
    lane: int = 1
    frm: int = 1          # lane we are switching from
    sw: int = 0           # ticks left in the lane switch
    y: int = 0
    vy: int = 0
    gnd: bool = True
    roll: int = 0
    cl: int = 1           # collision lane last tick

    def key(self):
        frm = self.frm if self.sw else self.lane
        return (self.lane, frm, self.sw, self.y, self.vy, self.gnd, self.roll)

    @staticmethod
    def from_key(k):
        lane, frm, sw, y, vy, gnd, roll = k
        p = Player(lane, frm, sw, y, vy, gnd, roll)
        p.cl = frm if sw > SWITCH_T // 2 else lane
        return p


def player_step(p, inp, env, jump_v=JUMP_V):
    """Advance the player one tick. Returns OK, CRASH or STUMBLE. Mirrors game.c exactly."""
    g, bar = env
    if (inp == LEFT or inp == RIGHT) and p.sw == 0:
        t = p.lane - 1 if inp == LEFT else p.lane + 1
        if 0 <= t < LANES:
            if g[t] > p.y + STEP_TOL or (bar[t] and p.y < HIGH_T and p.y + STAND_H > HIGH_B):
                return STUMBLE
            p.frm, p.lane, p.sw = p.lane, t, SWITCH_T
    elif inp == JUMP and p.gnd:
        p.vy, p.gnd, p.roll = jump_v, False, 0
    elif inp == ROLL:
        if p.gnd:
            p.roll = ROLL_T
        elif p.vy > -DIVE_V:
            p.vy, p.roll = -DIVE_V, ROLL_T
    if p.sw:
        p.sw -= 1
    cl = p.frm if p.sw > SWITCH_T // 2 else p.lane
    if cl != p.cl and g[cl] > p.y + STEP_TOL:
        return STUMBLE                      # ran into the side of something mid-switch
    p.cl = cl
    yprev = p.y
    p.y += p.vy
    p.vy -= G
    gc = g[cl]
    if p.y <= gc:
        if yprev < gc - STEP_TOL:
            return CRASH
        p.y, p.vy, p.gnd = gc, 0, True
    else:
        p.gnd = False
    if p.roll:
        p.roll -= 1
    h = ROLL_H if p.roll else STAND_H
    if bar[cl] and p.y < HIGH_T and p.y + h > HIGH_B:
        return CRASH
    return OK


# ------------------------------------------------------------------ random numbers and chainer
class Rng:
    """xorshift32 (13, 17, 5); the game uses the same generator."""

    def __init__(self, seed=0x2545F491):
        self.s = seed & 0xFFFFFFFF or 1

    def next(self):
        x = self.s
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.s = x
        return x

    def below(self, n):                     # 0..n-1, n < 65536 (16-bit divide on the 68000)
        return (self.next() >> 16) % n


class Chainer:
    """Picks the next pattern: weighted by `weight` among patterns whose tier speed is reached,
    never the same pattern twice in a row."""

    def __init__(self, pats, rng):
        self.pats, self.rng, self.last = pats, rng, -1

    def pick(self, v):
        cand = [i for i, p in enumerate(self.pats) if TIER_SPEED[p.tier] <= v and i != self.last]
        total = sum(self.pats[i].weight for i in cand)
        r = self.rng.below(total)
        for i in cand:
            r -= self.pats[i].weight
            if r < 0:
                self.last = i
                return i
        raise AssertionError


START_CLEAR = 12 * U        # empty track before the first pattern

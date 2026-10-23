#!/usr/bin/env python3
"""Rail Runner full game logic in Python: a line-by-line mirror of runner/game/game.c.

Used to (1) generate input replays with a backtracking autopilot and (2) predict exactly what
the 68000 game must show after playing them (score, coins, z, state): the MAME replay test
compares the two, which checks that game.c matches this file (and sim.py's proven physics).

Usage:
    game.py replay OUTDIR [--seconds 120] [--seed 0x2545F491]
        -> OUTDIR/replay.h (input table for the REPLAY build) and OUTDIR/replay.json (expected
           checkpoints: game tick -> score, coins, z, state, lane, y)
"""
import copy
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from params import *  # noqa: F401,F403
import sim  # noqa: E402

K_TRAIN, K_RAMP, K_LOW, K_HIGH, K_WALL, K_ONCOMING = range(6)
IN_NONE, IN_LEFT, IN_RIGHT, IN_JUMP, IN_ROLL, IN_BOARD = range(6)
EV_OK, EV_CRASH, EV_STUMBLE = range(3)
PU_NONE, PU_JETPACK, PU_SNEAKERS, PU_MAGNET, PU_MULT, PU_BOARD, N_PU = range(7)
ST_RUN, ST_CRASHED, ST_CAUGHT = range(3)
MAX_OBJ, MAX_COIN = 48, 112
COIN_Y, JET_FALL, SKY_LEAD, SKY_STOP = 300, 24, 14 * U, 100
TIER = sim.TIER_SPEED
M32 = 0xFFFFFFFF


def cdiv(a, b):
    """C integer division (truncates toward zero)."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


class GameState:
    pass


def load_tables():
    import json as _j
    pats = sim.load_patterns()
    coins = _j.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "coins.json")))
    return [(p.tier, p.weight, p.rows, p.objs, coins.get(p.name, [])) for p in pats]


PATS = load_tables()


def rnd(g):
    x = g.rng
    x ^= (x << 13) & M32
    x ^= x >> 17
    x ^= (x << 5) & M32
    g.rng = x
    return x


def below(g, n):
    return ((rnd(g) >> 16) & 0xFFFF) % n


def speed_at(z):
    v = V0 + cdiv(z, SPEED_STEP_ZU)
    return VMAX if v > VMAX else v


def gap_rows_c(v):
    return GAP_ROWS + cdiv(v - V0, GAP_PER_V)


def pick(g, v):
    total = 0
    for i, p in enumerate(PATS):
        if TIER[p[0]] <= v and i != g.last_pat:
            total += p[1]
    r = below(g, total)
    for i, p in enumerate(PATS):
        if TIER[p[0]] <= v and i != g.last_pat:
            r -= p[1]
            if r < 0:
                g.last_pat = i
                return i
    return 0


def spawn_pattern(g):
    v = speed_at(g.z)
    i = pick(g, v)
    tier, weight, rows, objs, coins = PATS[i]
    first_coin = len(g.coin)
    zs = g.next_pat
    for kind, lane, r0, r1 in objs:
        a, b = zs + r0 * U, zs + r1 * U
        if kind == K_LOW:
            a += (U - LOW_D) // 2; b = a + LOW_D
        elif kind == K_HIGH:
            a += (U - HIGH_D) // 2; b = a + HIGH_D
        elif kind == K_WALL:
            a += (U - WALL_D) // 2; b = a + WALL_D
        elif kind == K_ONCOMING:
            t_meet = cdiv(a - g.z, v)
            ln = b - a
            a = a + ONCOMING_V * t_meet
            b = a + ln
        if len(g.obj) >= MAX_OBJ:
            continue
        g.obj.append([kind, lane, a, b])
    for row, lane, y in coins:
        if len(g.coin) >= MAX_COIN:
            break
        g.coin.append([zs + row * U + U // 2, lane, y + COIN_Y, 0])
    if below(g, POWERUP_EVERY) == 0 and len(g.coin) > first_coin:
        n = len(g.coin) - first_coin
        j = below(g, n)
        g.coin[first_coin + j][3] = 1 + below(g, N_PU - 1)
    g.next_pat += (rows + gap_rows_c(v)) * U


def collision_lane(p):
    return p["frm"] if p["sw"] > SWITCH_T // 2 else p["lane"]


def env_at(g):
    lo, hi = g.z - PZ, g.z + PZ
    gnd = [0, 0, 0]
    bar = [0, 0, 0]
    for kind, lane, z0, z1 in g.obj:
        if z1 <= lo or z0 >= hi:
            continue
        if kind == K_HIGH:
            bar[lane] = 1
            continue
        if kind == K_RAMP:
            top = hi if hi < z1 else z1
            h = cdiv(TRAIN_H * (top - z0), z1 - z0)
        elif kind == K_LOW:
            h = LOW_H
        else:
            h = TRAIN_H
        if h > gnd[lane]:
            gnd[lane] = h
    return gnd, bar


def player_step(g, inp, gnd, bar):
    p = g.p
    jump_v = JUMP_V_HI if g.sneak_t else JUMP_V
    inv = g.invuln_t > 0 or g.jet_t > 0
    if (inp == IN_LEFT or inp == IN_RIGHT) and p["sw"] == 0:
        t = p["lane"] - 1 if inp == IN_LEFT else p["lane"] + 1
        if 0 <= t < LANES:
            if not inv and (gnd[t] > p["y"] + STEP_TOL or
                            (bar[t] and p["y"] < HIGH_T and p["y"] + STAND_H > HIGH_B)):
                return EV_STUMBLE
            p["frm"], p["lane"], p["sw"] = p["lane"], t, SWITCH_T
            g.events |= 64
    elif inp == IN_JUMP and p["gnd"]:
        p["vy"], p["gnd"], p["roll"] = jump_v, 0, 0
        g.events |= 2
    elif inp == IN_ROLL:
        if p["gnd"]:
            p["roll"] = ROLL_T
            g.events |= 4
        elif p["vy"] > -DIVE_V:
            p["vy"], p["roll"] = -DIVE_V, ROLL_T
            g.events |= 4
    if p["sw"]:
        p["sw"] -= 1
    cl = collision_lane(p)
    if cl != p["cl"] and not inv and gnd[cl] > p["y"] + STEP_TOL:
        return EV_STUMBLE
    p["cl"] = cl
    if g.jet_t > 0:
        if p["y"] < JET_Y:
            p["y"] += JET_RISE
            if p["y"] > JET_Y:
                p["y"] = JET_Y
        p["vy"] = 0
        p["gnd"] = 0
        p["roll"] = 0
        return EV_OK
    yprev = p["y"]
    p["y"] += p["vy"]
    p["vy"] -= G
    if inv and p["vy"] < -JET_FALL:
        p["vy"] = -JET_FALL
    gc = gnd[cl]
    if p["y"] <= gc:
        if yprev < gc - STEP_TOL and not inv:
            return EV_CRASH
        p["y"], p["vy"], p["gnd"] = gc, 0, 1
    else:
        p["gnd"] = 0
    if p["roll"]:
        p["roll"] -= 1
    h = ROLL_H if p["roll"] else STAND_H
    if not inv and bar[cl] and p["y"] < HIGH_T and p["y"] + h > HIGH_B:
        return EV_CRASH
    return EV_OK


def collect(g, c):
    mult = 2 if g.mult_t else 1
    g.events |= 32 if c[3] else 1
    t = c[3]
    if t == 0:
        g.coins += 1
        g.score += COIN_POINTS * mult
    elif t == PU_JETPACK:
        g.jet_t = JET_T
        g.next_sky = g.z + 4 * U
    elif t == PU_SNEAKERS:
        g.sneak_t = SNEAKERS_T
    elif t == PU_MAGNET:
        g.magnet_t = MAGNET_T
    elif t == PU_MULT:
        g.mult_t = MULT_T
    elif t == PU_BOARD:
        if g.boards < 9:
            g.boards += 1
    if t:
        g.pu_got = t


def game_init(seed=0x2545F491):
    g = GameState()
    g.z, g.v, g.ticks = 0, V0, 0
    g.p = dict(lane=1, frm=1, sw=0, y=0, vy=0, gnd=1, roll=0, cl=1)
    g.obj, g.coin = [], []
    g.next_pat, g.last_pat, g.rng = sim.START_CLEAR, -1, seed or 1
    g.state, g.state_t = ST_RUN, 0
    g.stumble_t = g.invuln_t = g.jet_t = g.sneak_t = g.magnet_t = g.mult_t = g.hover_t = 0
    g.chase_t = CHASE_START_T
    g.boards = 1
    g.score = g.coins = g.last_u = g.next_sky = 0
    g.pu_got = 0
    g.events = 0
    return g


def game_tick(g, inp):
    p = g.p
    g.ticks += 1
    g.events = 0
    if g.state != ST_RUN:
        g.state_t += 1
        return
    g.v = speed_at(g.z)
    g.z += g.v
    while g.z + DRAW_DIST >= g.next_pat:
        spawn_pattern(g)
    if g.jet_t > SKY_STOP:
        while g.z + SKY_LEAD >= g.next_sky:
            if len(g.coin) < MAX_COIN:
                g.coin.append([g.next_sky, ((g.next_sky >> 10) // 10) % 3, JET_Y + COIN_Y, 0])
            g.next_sky += U
    i = 0
    while i < len(g.obj):
        o = g.obj[i]
        if o[0] == K_ONCOMING:
            o[2] -= ONCOMING_V
            o[3] -= ONCOMING_V
        if o[3] <= g.z - 4 * U:
            g.obj[i] = g.obj[-1]
            g.obj.pop()
            continue
        i += 1
    if inp == IN_BOARD:
        if g.boards > 0 and g.hover_t == 0:
            g.boards -= 1
            g.hover_t = HOVER_T
            g.events |= 32
        inp = IN_NONE
    gnd, bar = env_at(g)
    ev = player_step(g, inp, gnd, bar)
    if ev == EV_STUMBLE:
        if p["sw"]:
            p["lane"] = p["frm"]
            p["sw"] = 0
            p["cl"] = p["lane"]
        g.stumble_t = STUMBLE_T
        g.events |= 16
        if g.chase_t > 0:
            g.state, g.state_t = ST_CAUGHT, 0
            return
        g.chase_t = CHASE_T
    elif ev == EV_CRASH:
        if g.hover_t > 0:
            g.hover_t = 0
            g.invuln_t = INVULN_T
            p["y"] = gnd[p["cl"]]
            p["vy"] = 0
            p["gnd"] = 1
            g.events |= 8
        else:
            g.state, g.state_t = ST_CRASHED, 0
            g.events |= 8
            return
    cl = p["cl"]
    i = 0
    while i < len(g.coin):
        c = g.coin[i]
        dz = c[0] - g.z
        if dz < -COIN_R_Z:
            g.coin[i] = g.coin[-1]
            g.coin.pop()
            continue
        if dz <= COIN_R_Z:
            dy = abs(c[2] - (p["y"] + COIN_Y))
            if g.magnet_t and c[3] == 0:
                hit = True
            else:
                hit = c[1] == cl and dy <= COIN_R_Y
            if hit:
                collect(g, c)
                g.coin[i] = g.coin[-1]
                g.coin.pop()
                continue
        i += 1
    if g.stumble_t: g.stumble_t -= 1
    if g.chase_t: g.chase_t -= 1
    if g.invuln_t: g.invuln_t -= 1
    if g.jet_t:
        g.jet_t -= 1
        if g.jet_t == 0:
            g.invuln_t = INVULN_T
            p["vy"] = 0
    if g.sneak_t: g.sneak_t -= 1
    if g.magnet_t: g.magnet_t -= 1
    if g.mult_t: g.mult_t -= 1
    if g.hover_t: g.hover_t -= 1
    u = g.z >> 10
    mult = 2 if g.mult_t else 1
    while g.last_u < u:
        g.last_u += 1
        g.score += mult


# ------------------------------------------------------------------ autopilot (replay generator)
DECIDE = 6
PREF = (IN_NONE, IN_JUMP, IN_LEFT, IN_RIGHT, IN_ROLL)


def snapshot(g):
    return copy.deepcopy(g)


def play_plan(seconds, seed):
    """Depth-first search with backtracking over decisions every DECIDE ticks. Returns the input
    list (one per tick) of a run that survives `seconds` without crashing or stumbling."""
    total = int(seconds * 60)
    n_dec = total // DECIDE
    g = game_init(seed)
    stack = [(snapshot(g), 0)]          # (state at decision d, next choice index)
    choices = []
    dead = set()
    nodes = 0
    while len(choices) < n_dec:
        st, ci = stack[-1]
        key = (len(choices), tuple(st.p.values()), st.jet_t > 0, st.sneak_t > 0, st.invuln_t > 0)
        if ci >= len(PREF) or key in dead:
            dead.add(key)
            stack.pop()
            if not choices:
                raise SystemExit("no surviving plan")
            choices.pop()
            continue
        stack[-1] = (st, ci + 1)
        g = snapshot(st)
        inp = PREF[ci]
        ok = True
        for t in range(DECIDE):
            game_tick(g, inp if t == 0 else IN_NONE)
            if g.state != ST_RUN or g.events & 16:
                ok = False
                break
        nodes += 1
        if ok:
            choices.append(inp)
            stack.append((g, 0))
    inputs = []
    for c in choices:
        inputs += [c] + [IN_NONE] * (DECIDE - 1)
    return inputs, nodes


def main():
    if len(sys.argv) < 3 or sys.argv[1] != "replay":
        print(__doc__)
        return
    out = sys.argv[2]
    seconds = 120
    seed = 0x2545F491
    if "--seconds" in sys.argv:
        seconds = float(sys.argv[sys.argv.index("--seconds") + 1])
    inputs, nodes = play_plan(seconds, seed)
    g = game_init(seed)
    checkpoints = {}
    for t, inp in enumerate(inputs):
        game_tick(g, inp)
        if (t + 1) % 600 == 0 or t + 1 == len(inputs):
            checkpoints[t + 1] = dict(score=g.score, coins=g.coins, z=g.z, state=g.state,
                                      lane=g.p["lane"], y=g.p["y"], boards=g.boards)
    ev = [(t + 1, inp) for t, inp in enumerate(inputs) if inp != IN_NONE]
    with open(os.path.join(out, "replay.h"), "w") as f:
        f.write("/* Generated by runner/proto/game.py replay: autopilot inputs (game tick, input). */\n")
        f.write(f"#define REPLAY_LEN {len(ev)}\n")
        f.write("static const long replay_tick[] = {" + ", ".join(str(t) for t, _ in ev) + "};\n")
        f.write("static const unsigned char replay_in[] = {" + ", ".join(str(i) for _, i in ev) + "};\n")
    json.dump(dict(seed=seed, ticks=len(inputs), checkpoints=checkpoints),
              open(os.path.join(out, "replay.json"), "w"), indent=1)
    last = checkpoints[len(inputs)]
    print(f"replay: {len(inputs)} ticks, {len(ev)} inputs, search nodes {nodes}; final {last}")


if __name__ == "__main__":
    main()

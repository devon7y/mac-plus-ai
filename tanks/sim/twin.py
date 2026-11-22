"""Tank Duel: the vectorised numpy twin of sim.c (N games at once, bit-identical to the C).

Every function here mirrors the one of the same name in sim.c, line by line, vectorised over the
N games (envs). Tanks are processed in index order and shells in slot order, as in C, so the
results are identical; test_bitexact.py checks this against the C compiled on the host.

All arithmetic is int32 (int64 for the hash and the random generator); right shifts floor, as C's
do on these compilers; the only divisions are of non-negative numbers.
"""
import numpy as np

from defs import (TW, TH, TS, MAXT, MAXS, HB, HIT, SHELL_HIT, SHELL_LIFE, BARREL, RAYSTEP, RAYN, NRAY,
                  AIMSTEP, AIMN, AIMHIT, NOBS, SIN, ATAN, TYPES, BOT_SITTER, BOT_WANDER, arena, ARENAS, GUMBEL_Q8,
                  HEADS)

I32 = np.int32
SIN_A = np.array(SIN, dtype=I32)
COS_A = np.array([SIN[(a + 64) & 255] for a in range(256)], dtype=I32)
ATAN_A = np.array(ATAN, dtype=I32)
TYPE_A = np.array([t[1:] for t in TYPES], dtype=I32)          # (NTYPE, 8)
MAXF, MAXR, ACC, TURN, TRATE, SSPEED, COOL, NMAX = range(8)
TUR_OFF = np.array([-24, -8, -2, 0, 2, 8, 24], dtype=I32)
GUMBEL_A = np.array(GUMBEL_Q8, dtype=np.int64)
ARENA_MAPS = np.array([arena(i)[1] for i in range(len(ARENAS))], dtype=np.uint8)   # (NA, TH, TW)
ARENA_SPAWNS = [arena(i)[2] for i in range(len(ARENAS))]

TANK_FIELDS = ("alive", "type", "team", "shield", "x", "y", "speed", "cool", "nshells", "by", "btimer",
               "hull", "tur", "bhead", "drive", "turn", "tgt", "fire")
SHELL_FIELDS = ("alive", "owner", "bounces", "age", "x", "y", "vx", "vy")


def s8(v):
    """(signed char) of an int array"""
    return ((v + 128) & 255) - 128


def mulq(v, q):
    """(v * q + 8192) >> 14, the rounded Q14 product"""
    return (v * q + 8192) >> 14


def clamp127(v):
    return np.clip(v, -127, 127)


class Twin:
    def __init__(self, n, nt, ns=MAXS):
        """n games with nt tanks each and ns shell slots (ns may be less than MAXS if no more than
        ns shells can ever be alive at once: slots are taken lowest first, as in C)"""
        self.n, self.nt, self.ns = n, nt, ns
        self.idx = np.arange(n)
        self.map = np.zeros((n, TH, TW), dtype=np.uint8)
        self.flat = np.ones(n * TH * 32, dtype=np.uint8)      # the walls, rows padded to 32 (fast lookups)
        self.base = (np.arange(n) * (TH * 32)).astype(np.int64)
        self.t = {f: np.zeros((n, nt), dtype=I32) for f in TANK_FIELDS}
        self.s = {f: np.zeros((n, ns), dtype=I32) for f in SHELL_FIELDS}
        self.tick = np.zeros(n, dtype=np.int64)
        self.rng = np.ones(n, dtype=np.int64)
        self.kill_bounce = np.zeros((n, nt), bool)    # (training only, not in C) killed by a bounced shell

    # ------------------------------------------------------------------ setup
    def init(self, arenas, seeds, mask=None):
        """sim_init for the games in mask (all if None): arenas, seeds: int arrays of length n"""
        m = np.ones(self.n, bool) if mask is None else mask
        arenas = np.broadcast_to(np.asarray(arenas), (self.n,))
        seeds = np.broadcast_to(np.asarray(seeds, dtype=np.int64), (self.n,))
        self.map[m] = ARENA_MAPS[arenas[m]]
        self.sync_map(m)
        for f in TANK_FIELDS:
            self.t[f][m] = 0
        self.t["by"][m] = -1
        for f in SHELL_FIELDS:
            self.s[f][m] = 0
        self.tick[m] = 0
        self.kill_bounce[m] = False
        self.rng[m] = np.where(seeds[m] != 0, seeds[m] & 0xFFFFFFFF, 1)

    def set_tank(self, i, typ, team, x, y, a, mask=None):
        """sim_add_tank for slot i at SU position (x, y) (tile centres: tx * 256 + 128)"""
        m = np.ones(self.n, bool) if mask is None else mask
        t = self.t
        vals = dict(alive=1, type=typ, team=team, shield=0, x=x, y=y, speed=0, cool=0, nshells=0, by=-1,
                    btimer=0, hull=a, tur=a, bhead=a, drive=0, turn=0, tgt=a, fire=0)
        for f, v in vals.items():
            v = np.broadcast_to(np.asarray(v, dtype=I32), (self.n,))
            t[f][m, i] = v[m]

    def sync_map(self, m=None):
        """copy self.map into the padded flat copy (call after changing self.map directly)"""
        f = self.flat.reshape(self.n, TH, 32)
        if m is None:
            f[:, :, :TW] = self.map
        else:
            f[m, :, :TW] = self.map[m]

    # ------------------------------------------------------------------ helpers
    def wall(self, x, y):
        """map[y >> 8][x >> 8] for arrays shaped (n, ...), as ((y >> 3) & ~31) + (x >> 8) in the
        32-wide rows; lanes outside the map (only unused ones) read some other cell"""
        shape = x.shape
        b = self.base.reshape((self.n,) + (1,) * (len(shape) - 1))
        return np.take(self.flat, b + ((y >> 3) & ~31) + (x >> 8), mode="clip").astype(bool)

    def rng_next(self, m):
        s = self.rng.copy()
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        self.rng = np.where(m, s, self.rng)
        return s

    def blocked(self, x, y, i):
        bx0 = np.clip((x - HB) >> 8, 0, TW - 1)
        bx1 = np.clip((x + HB - 1) >> 8, 0, TW - 1)
        by0 = np.clip((y - HB) >> 8, 0, TH - 1)
        by1 = np.clip((y + HB - 1) >> 8, 0, TH - 1)
        mp, n = self.map, self.idx
        b = (mp[n, by0, bx0] | mp[n, by0, bx1] | mp[n, by1, bx0] | mp[n, by1, bx1]).astype(bool)
        t = self.t
        for j in range(self.nt):
            if j == i:
                continue
            b |= (t["alive"][:, j] != 0) & (np.abs(x - t["x"][:, j]) < 2 * HB) & (np.abs(y - t["y"][:, j]) < 2 * HB)
        return b

    def kill_shells(self, die):
        """kill_shell for every (game, slot) in the boolean mask die (n, ns)"""
        s, t = self.s, self.t
        s["alive"][die] = 0
        for i in range(self.nt):
            t["nshells"][:, i] -= np.sum(die & (s["owner"] == i), axis=1).astype(I32)

    # ------------------------------------------------------------------ tick
    def tank_update(self, i):
        t, s = self.t, self.s
        al = t["alive"][:, i] != 0
        ty = TYPE_A[t["type"][:, i]]
        sh = t["shield"][:, i]
        t["shield"][:, i] = np.where(al & (sh > 0), sh - 1, sh)
        hull = np.where(al, (t["hull"][:, i] + t["turn"][:, i] * ty[:, TURN]) & 255, t["hull"][:, i])
        t["hull"][:, i] = hull
        drv = t["drive"][:, i]
        target = np.where(drv > 0, ty[:, MAXF], np.where(drv < 0, -ty[:, MAXR], 0))
        sp = t["speed"][:, i]
        up = np.minimum(sp + ty[:, ACC], target)
        dn = np.maximum(sp - ty[:, ACC], target)
        sp = np.where(al, np.where(sp < target, up, np.where(sp > target, dn, sp)), sp)
        t["speed"][:, i] = sp
        dx = mulq(sp, COS_A[hull])
        dy = mulq(sp, SIN_A[hull])
        x, y = t["x"][:, i], t["y"][:, i]
        nx = x + dx
        mv = al & (dx != 0)
        mv &= ~self.blocked(nx, y, i)
        x = np.where(mv, nx, x)
        t["x"][:, i] = x
        ny = y + dy
        mv = al & (dy != 0)
        mv &= ~self.blocked(x, ny, i)
        y = np.where(mv, ny, y)
        t["y"][:, i] = y
        tur, tgt, tr = t["tur"][:, i], t["tgt"][:, i], ty[:, TRATE]
        d = s8(tgt - tur)
        nt = np.where(d > tr, (tur + tr) & 255, np.where(d < -tr, (tur - tr) & 255, tgt))
        tur = np.where(al, nt, tur)
        t["tur"][:, i] = tur
        cool = t["cool"][:, i]
        cool = np.where(al & (cool > 0), cool - 1, cool)
        fire = al & (t["fire"][:, i] != 0) & (cool == 0) & (t["nshells"][:, i] < ty[:, NMAX])
        t["cool"][:, i] = np.where(fire, ty[:, COOL], cool)
        if not fire.any():
            return
        sx = x + mulq(BARREL, COS_A[tur])
        sy = y + mulq(BARREL, SIN_A[tur])
        ok = fire & ~self.wall(sx, sy)
        free = s["alive"] == 0
        k = np.argmax(free, axis=1)
        ok &= free[self.idx, k]
        if not ok.any():
            return
        n = self.idx[ok]
        k = k[ok]
        s["alive"][n, k] = 1
        s["owner"][n, k] = i
        s["bounces"][n, k] = 0
        s["age"][n, k] = 0
        s["x"][n, k] = sx[ok]
        s["y"][n, k] = sy[ok]
        s["vx"][n, k] = mulq(ty[ok, SSPEED], COS_A[tur[ok]])
        s["vy"][n, k] = mulq(ty[ok, SSPEED], SIN_A[tur[ok]])
        t["nshells"][n, i] += 1

    def shells_move(self):
        s = self.s
        al = s["alive"] != 0
        s["age"] = np.where(al, s["age"] + 1, s["age"])
        exp = al & (s["age"] > SHELL_LIFE)
        if exp.any():
            self.kill_shells(exp)
        al &= ~exp
        x, y, vx, vy = s["x"], s["y"], s["vx"], s["vy"]
        m = x + vx
        wx = al & self.wall(m, y)
        s["vx"] = np.where(wx, -vx, vx)
        x = np.where(al & ~wx, m, x)
        s["x"] = x
        m = y + vy
        wy = al & self.wall(x, m)
        s["vy"] = np.where(wy, -vy, vy)
        s["y"] = np.where(al & ~wy, m, y)
        hit = wx | wy
        s["bounces"] = np.where(hit, s["bounces"] + 1, s["bounces"])
        die = hit & (s["bounces"] > 1)
        if die.any():
            self.kill_shells(die)

    def shells_vs_shells(self):
        s = self.s
        ns = self.ns
        for a in range(ns - 1):
            pa = s["alive"][:, a] != 0
            if not pa.any():
                continue
            q = s["alive"][:, a + 1:] != 0
            close = (pa[:, None] & q & (np.abs(s["x"][:, a:a + 1] - s["x"][:, a + 1:]) < SHELL_HIT)
                     & (np.abs(s["y"][:, a:a + 1] - s["y"][:, a + 1:]) < SHELL_HIT))
            hit = close.any(axis=1)
            if not hit.any():
                continue
            b = np.argmax(close, axis=1) + a + 1
            die = np.zeros((self.n, ns), bool)
            n = self.idx[hit]
            die[n, a] = True
            die[n, b[hit]] = True
            self.kill_shells(die)

    def shells_vs_tanks(self):
        s, t = self.s, self.t
        for k in range(self.ns):
            sa = s["alive"][:, k] != 0
            if not sa.any():
                continue
            sx, sy, own, bo = s["x"][:, k], s["y"][:, k], s["owner"][:, k], s["bounces"][:, k]
            cand = np.zeros((self.n, self.nt), bool)
            for i in range(self.nt):
                cand[:, i] = (sa & (t["alive"][:, i] != 0) & (np.abs(sx - t["x"][:, i]) < HIT)
                              & (np.abs(sy - t["y"][:, i]) < HIT) & ~((own == i) & (bo == 0)))
            hit = cand.any(axis=1)
            if not hit.any():
                continue
            i = np.argmax(cand, axis=1)
            die = np.zeros((self.n, self.ns), bool)
            die[hit, k] = True
            self.kill_shells(die)
            n = self.idx
            shield = t["shield"][n, i] > 0
            same = (i != own) & (t["team"][n, i] == t["team"][n, own])
            dead = hit & ~shield & ~same
            nn, ii = n[dead], i[dead]
            t["alive"][nn, ii] = 0
            t["by"][nn, ii] = own[dead]
            self.kill_bounce[nn, ii] = bo[dead] > 0

    def step(self):
        """sim_tick"""
        for i in range(self.nt):
            self.tank_update(i)
        self.shells_move()
        self.shells_vs_shells()
        self.shells_vs_tanks()
        self.tick += 1

    # ------------------------------------------------------------------ sensing
    @staticmethod
    def atan2(dy, dx):
        ax, ay = np.abs(dx), np.abs(dy)
        big = np.maximum(ax, 1)
        bgy = np.maximum(ay, 1)
        r1 = ATAN_A[np.clip((ay << 6) // big, 0, 64)]
        r2 = 64 - ATAN_A[np.clip((ax << 6) // bgy, 0, 64)]
        r = np.where(ax >= ay, r1, r2)
        a = np.where(dx >= 0, np.where(dy >= 0, r, 256 - r), np.where(dy >= 0, 128 - r, 128 + r)) & 255
        return np.where((ax == 0) & (ay == 0), 0, a)

    def ray(self, x, y, a):
        """sim_ray for arrays shaped (n, ...)"""
        dx = mulq(RAYSTEP, COS_A[a])
        dy = mulq(RAYSTEP, SIN_A[a])
        res = np.full(x.shape, RAYN + 1, dtype=I32)
        done = np.zeros(x.shape, bool)
        for n in range(1, RAYN + 1):
            x = x + dx
            y = y + dy
            w = self.wall(x, y) & ~done
            res[w] = n
            done |= w
            if done.all():
                break
        return res

    def los(self, ax, ay, bx, by):
        dx, dy = bx - ax, by - ay
        adx, ady = np.abs(dx), np.abs(dy)
        n = (np.maximum(adx, ady) >> 7) + 1
        sx = (adx << 4) // n
        sy = (ady << 4) // n
        sx = np.where(dx < 0, -sx, sx)
        sy = np.where(dy < 0, -sy, sy)
        px, py = ax << 4, ay << 4
        blocked = np.zeros(ax.shape, bool)
        nmax = int(n.max()) if n.size else 1
        for k in range(1, nmax):
            act = k < n
            px = px + sx
            py = py + sy
            w = np.take(self.flat, self.base + ((py >> 7) & ~31) + (px >> 12), mode="clip").astype(bool)
            blocked |= act & w
        return ~blocked

    def aim(self, i, j, valid, pts=False):
        """sim_aim of tank i towards tank j (int array per game); returns hit, self[, path]"""
        t = self.t
        n = self.idx
        tur = t["tur"][:, i]
        mx, my = t["x"][:, i], t["y"][:, i]
        tx, ty = t["x"][n, j], t["y"][n, j]
        x = mx + mulq(BARREL, COS_A[tur])
        y = my + mulq(BARREL, SIN_A[tur])
        vx = mulq(AIMSTEP, COS_A[tur])
        vy = mulq(AIMSTEP, SIN_A[tur])
        hit = np.zeros(self.n, I32)
        slf = np.zeros(self.n, I32)
        b = np.zeros(self.n, I32)
        done = ~valid | self.wall(x, y)
        path = [(x.copy(), y.copy(), ~done)] if pts else None
        for _ in range(AIMN):
            act = ~done
            if not act.any():
                break
            m = x + vx
            wx = act & self.wall(m, y)
            vx = np.where(wx, -vx, vx)
            x = np.where(act & ~wx, m, x)
            m = y + vy
            wy = act & self.wall(x, m)
            vy = np.where(wy, -vy, vy)
            y = np.where(act & ~wy, m, y)
            if pts:
                path.append((x.copy(), y.copy(), act.copy()))
            h = wx | wy
            b = np.where(h, b + 1, b)
            stop = h & (b > 1)
            done |= stop
            act &= ~stop
            th = act & (np.abs(x - tx) < AIMHIT) & (np.abs(y - ty) < AIMHIT)
            hit = np.where(th, np.where(b > 0, 64, 127), hit)
            done |= th
            act &= ~th
            sh = act & (b > 0) & (np.abs(x - mx) < AIMHIT) & (np.abs(y - my) < AIMHIT)
            slf = np.where(sh, 127, slf)
            done |= sh
        if pts:
            return hit, slf, path
        return hit, slf

    def nearest(self, i, enemies):
        """sim_target (enemies=True) or the nearest ally: index (0 if none) and valid flag"""
        t = self.t
        mx, my, team = t["x"][:, i], t["y"][:, i], t["team"][:, i]
        d = np.full((self.n, self.nt), 1 << 20, dtype=np.int64)
        for j in range(self.nt):
            if j == i:
                continue
            ok = (t["alive"][:, j] != 0) & ((t["team"][:, j] != team) if enemies else (t["team"][:, j] == team))
            dx = np.abs((t["x"][:, j] - mx) >> 4)
            dy = np.abs((t["y"][:, j] - my) >> 4)
            d[:, j] = np.where(ok, np.maximum(dx, dy), 1 << 20)
        best = np.argmin(d, axis=1)
        valid = d[self.idx, best] < (1 << 20)
        return np.where(valid, best, 0), valid

    def obs(self, i):
        """sim_obs of tank i: (n, NOBS) int32"""
        t, s, n = self.t, self.s, self.idx
        o = np.zeros((self.n, NOBS), dtype=I32)
        h = t["hull"][:, i]
        C, Sn = COS_A[h][:, None], SIN_A[h][:, None]
        C1, S1 = C[:, 0], Sn[:, 0]

        def rotf(dx, dy):
            return (dx * C1 + dy * S1) >> 14

        def rots(dx, dy):
            return (dy * C1 - dx * S1) >> 14
        mx, my = t["x"][:, i], t["y"][:, i]
        a = (h[:, None] + 32 * np.arange(NRAY)[None, :]) & 255
        o[:, 0:NRAY] = self.ray(np.repeat(mx[:, None], NRAY, 1), np.repeat(my[:, None], NRAY, 1), a) * 6
        o[:, 8] = t["speed"][:, i] * 5
        r = (t["tur"][:, i] - h) & 255
        o[:, 9] = clamp127(SIN_A[r] >> 7)
        o[:, 10] = clamp127(COS_A[r] >> 7)
        o[:, 11] = np.minimum(t["cool"][:, i] * 4, 127)
        o[:, 12] = (TYPE_A[t["type"][:, i], NMAX] - t["nshells"][:, i]) * 40
        o[:, 13] = np.where(t["shield"][:, i] > 0, 127, 0)

        j, valid = self.nearest(i, True)
        tx, ty_ = t["x"][n, j], t["y"][n, j]
        dxp, dyp = (tx - mx) >> 4, (ty_ - my) >> 4
        tsp, th = t["speed"][n, j], t["hull"][n, j]
        tvx = (tsp * COS_A[th]) >> 14
        tvy = (tsp * SIN_A[th]) >> 14
        at = self.atan2(dyp, dxp)
        r = (at - t["tur"][:, i]) & 255
        d = np.maximum(np.abs(dxp), np.abs(dyp))
        hit, slf = self.aim(i, j, valid)
        r2 = (at + 128 - t["tur"][n, j]) & 255
        tcan = (t["cool"][n, j] == 0) & (t["nshells"][n, j] < TYPE_A[t["type"][n, j], NMAX])
        vals = [clamp127(rotf(dxp, dyp) >> 2), clamp127(rots(dxp, dyp) >> 2),
                clamp127(rotf(tvx, tvy) * 5), clamp127(rots(tvx, tvy) * 5),
                clamp127(SIN_A[r] >> 7), clamp127(COS_A[r] >> 7),
                np.minimum(d >> 2, 127),
                np.where(self.los(mx, my, tx, ty_), 127, 0),
                hit, slf,
                clamp127(COS_A[r2] >> 7),
                np.where(tcan, 127, 0),
                np.full(self.n, 127)]
        for k, v in enumerate(vals):
            o[:, 14 + k] = np.where(valid, v, 0)

        j, valid = self.nearest(i, False)
        dxp, dyp = (t["x"][n, j] - mx) >> 4, (t["y"][n, j] - my) >> 4
        o[:, 27] = np.where(valid, clamp127(rotf(dxp, dyp) >> 2), 0)
        o[:, 28] = np.where(valid, clamp127(rots(dxp, dyp) >> 2), 0)
        o[:, 29] = np.where(valid, 127, 0)

        ok = (s["alive"] != 0) & ~((s["owner"] == i) & (s["bounces"] == 0))
        dx = np.abs((s["x"] - mx[:, None]) >> 4)
        dy = np.abs((s["y"] - my[:, None]) >> 4)
        key = np.where(ok, np.maximum(dx, dy) * 64 + np.arange(self.ns)[None, :], 1 << 30)
        for k in range(2):
            b = np.argmin(key, axis=1)
            v = key[n, b] < (1 << 30)
            key[n, b] = 1 << 30
            sxp = (s["x"][n, b] - mx) >> 4
            syp = (s["y"][n, b] - my) >> 4
            svx, svy = s["vx"][n, b], s["vy"][n, b]
            q = 30 + 5 * k
            o[:, q] = np.where(v, 127, 0)
            o[:, q + 1] = np.where(v, clamp127(rotf(sxp, syp)), 0)
            o[:, q + 2] = np.where(v, clamp127(rots(sxp, syp)), 0)
            o[:, q + 3] = np.where(v, clamp127((rotf(svx, svy) * 3) >> 1), 0)
            o[:, q + 4] = np.where(v, clamp127((rots(svx, svy) * 3) >> 1), 0)
        return o

    def pick(self, z, shift, mask=None):
        """sim_pick: actions (n, 4) from integer logits z (n, 15); the rng advances only in mask"""
        m = np.ones(self.n, bool) if mask is None else mask
        z = np.asarray(z, dtype=np.int64)
        act = np.zeros((self.n, 4), dtype=np.int32)
        o = nb = 0
        w = np.zeros(self.n, dtype=np.int64)
        shift = np.broadcast_to(np.asarray(shift, dtype=np.int64), (self.n,))
        for h, nh in enumerate(HEADS):
            best = np.zeros(self.n, dtype=np.int64)
            arg = np.zeros(self.n, dtype=np.int32)
            for k in range(nh):
                if not (nb & 3):
                    w = self.rng_next(m)
                v = z[:, o + k] + (GUMBEL_A[w & 255] << shift)
                w = w >> 8
                nb += 1
                if k == 0:
                    best = v
                else:
                    better = v > best
                    best = np.where(better, v, best)
                    arg = np.where(better, k, arg)
            act[:, h] = arg
            o += nh
        return act

    def apply_action(self, i, act, mask=None):
        """sim_apply_action: act (n, 4) ints (drive 0..2, turn 0..2, turret 0..6, fire 0..1)"""
        m = np.ones(self.n, bool) if mask is None else mask
        t = self.t
        t["drive"][m, i] = act[m, 0] - 1
        t["turn"][m, i] = act[m, 1] - 1
        t["tgt"][m, i] = (t["tur"][m, i] + TUR_OFF[act[m, 2]]) & 255
        t["fire"][m, i] = act[m, 3]

    # ------------------------------------------------------------------ scripted bots
    def bot_act(self, i, kind, mask=None):
        m = np.ones(self.n, bool) if mask is None else mask.copy()
        t, n = self.t, self.idx
        j, valid = self.nearest(i, True)
        none = m & ~valid
        t["drive"][none, i] = 0
        t["turn"][none, i] = 0
        t["fire"][none, i] = 0
        t["tgt"][none, i] = t["tur"][none, i]
        m &= valid
        if not m.any():
            return
        mx, my = t["x"][:, i], t["y"][:, i]
        dxp, dyp = (t["x"][n, j] - mx) >> 4, (t["y"][n, j] - my) >> 4
        dist = np.maximum(np.abs(dxp), np.abs(dyp))
        at = self.atan2(dyp, dxp)
        t["tgt"][m, i] = at[m]
        vis = self.los(mx, my, t["x"][n, j], t["y"][n, j])
        d = s8(at - t["tur"][:, i])
        t["fire"][m, i] = (vis & (d >= -4) & (d <= 4))[m]
        if kind == BOT_SITTER:
            t["drive"][m, i] = 0
            t["turn"][m, i] = 0
            return
        if kind == BOT_WANDER:
            bt = t["btimer"][:, i].copy()
            new = m & (bt <= 0)
            r = self.rng_next(new)
            t["bhead"][new, i] = (r & 255)[new].astype(I32)
            t["btimer"][new, i] = (20 + ((r >> 8) & 31))[new].astype(I32)
            dec = m & ~(bt <= 0)
            t["btimer"][dec, i] -= 1
            want = t["bhead"][:, i]
        else:
            want = at
        hull = t["hull"][:, i]
        fwd = self.ray(mx, my, hull)
        near = m & (fwd <= 2)
        far = m & ~(fwd <= 2)
        if near.any():
            lr = self.ray(np.stack([mx, mx], 1), np.stack([my, my], 1),
                          np.stack([(hull - 32) & 255, (hull + 32) & 255], 1))
            right = lr[:, 1] >= lr[:, 0]
            t["turn"][near, i] = np.where(right, 1, -1)[near]
            t["drive"][near, i] = np.where(fwd <= 1, -1, 0)[near]
            if kind == BOT_WANDER:
                t["bhead"][near, i] = ((hull + np.where(right, 64, -64)) & 255)[near]
        dd = s8(want - hull)
        t["turn"][far, i] = np.where(dd > 6, 1, np.where(dd < -6, -1, 0))[far]
        go = (dd > -48) & (dd < 48) & ((kind == BOT_WANDER) | (dist > 64) | ~vis)
        t["drive"][far, i] = np.where(go, 1, 0)[far]

    # ------------------------------------------------------------------ hash
    def hash(self):
        def mix(h, v):
            return (((h << 5) | (h >> 27)) & 0xFFFFFFFF) ^ (v.astype(np.int64) & 0xFFFF)
        t, s = self.t, self.s
        h = self.tick.astype(np.int64) & 0xFFFFFFFF
        for i in range(self.nt):
            for f in ("alive", "x", "y", "speed", "hull", "tur", "cool", "nshells", "shield", "by"):
                h = mix(h, t[f][:, i])
        for k in range(self.ns):
            al = s["alive"][:, k] != 0
            g = mix(h, np.full(self.n, k))
            for f in ("x", "y", "vx", "vy", "bounces", "age", "owner"):
                g = mix(g, s[f][:, k])
            h = np.where(al, g, h)
        h = mix(h, self.rng & 0xFFFF)
        return h

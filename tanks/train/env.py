"""Training environment: N duels at once on the numpy twin (bit-identical to the Mac's simulation).

The learning tank is tank 0 (it decides at ticks 0, 3, 6, ...); tank 1 is its opponent (a player-type
tank driven by a scripted bot or a frozen policy from the league pool, deciding at ticks 1, 4, ...).
With nt=3, tank 2 is an ally of the learner (same type, driven by a frozen policy or a bot).
One step = one decision of the learner = 3 simulation ticks.

Arenas: the game's 8 arenas with their mirror images, and procedurally generated ones.
Rewards: +1 win, -1 loss, -0.5 trade, plus the tank type's style terms (STYLE).
"""
import os
import sys

import numpy as np
import torch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "sim"))
import defs as D  # noqa: E402
from twin import Twin, ARENA_MAPS  # noqa: E402

EP_TICKS = 1800          # 60 s

# per tank type: timeout reward, approach weight, distance band (px) and its penalty per step,
# exposure penalty per step, bank-shot kill bonus, cost per shot
STYLE = {
    D.T_RUSHER: dict(timeout=-0.6, approach=0.0006, far=110, far_pen=0.002, near=0, near_pen=0.0,
                     exposed=0.0, bank=0.0, shot=0.0),
    D.T_SNIPER: dict(timeout=-0.2, approach=0.0, far=10_000, far_pen=0.0, near=130, near_pen=0.003,
                     exposed=0.0, bank=0.4, shot=0.003),
    D.T_CAREFUL: dict(timeout=0.0, approach=0.0, far=10_000, far_pen=0.0, near=0, near_pen=0.0,
                      exposed=0.004, bank=0.1, shot=0.0),
    D.T_PLAYER: dict(timeout=-0.3, approach=0.0, far=10_000, far_pen=0.0, near=0, near_pen=0.0,
                     exposed=0.001, bank=0.0, shot=0.0),
}


def connected(grid, a, b):
    seen = np.zeros_like(grid, dtype=bool)
    st = [a]
    seen[a[1], a[0]] = True
    while st:
        x, y = st.pop()
        if (x, y) == b:
            return True
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if not grid[ny, nx] and not seen[ny, nx]:
                seen[ny, nx] = True
                st.append((nx, ny))
    return False


def random_arena(rng):
    g = np.zeros((D.TH, D.TW), dtype=np.uint8)
    g[0, :] = g[-1, :] = g[:, 0] = g[:, -1] = 1
    for _ in range(rng.integers(4, 14)):
        if rng.random() < 0.5:   # bar
            ln = rng.integers(2, 8)
            horiz = rng.random() < 0.5
            x, y = rng.integers(2, D.TW - 2), rng.integers(2, D.TH - 2)
            if horiz:
                g[y, x:min(D.TW - 1, x + ln)] = 1
            else:
                g[y:min(D.TH - 1, y + ln), x] = 1
        else:                    # block
            w, h = rng.integers(1, 4), rng.integers(1, 3)
            x, y = rng.integers(2, D.TW - 2 - w), rng.integers(2, D.TH - 2 - h)
            g[y:y + h, x:x + w] = 1
    return g


def components(g):
    """label of each floor tile's connected region (0 = wall)"""
    lab = np.zeros(g.shape, np.int32)
    k = 0
    for y0 in range(g.shape[0]):
        for x0 in range(g.shape[1]):
            if g[y0, x0] or lab[y0, x0]:
                continue
            k += 1
            st = [(x0, y0)]
            lab[y0, x0] = k
            while st:
                x, y = st.pop()
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if not g[ny, nx] and not lab[ny, nx]:
                        lab[ny, nx] = k
                        st.append((nx, ny))
    return lab


def arena_pool(rng, n_random=300):
    maps = []
    for m in ARENA_MAPS:
        maps += [m, m[:, ::-1], m[::-1, :], m[::-1, ::-1]]
    game = len(maps)
    for _ in range(n_random):
        maps.append(random_arena(rng))
    return np.ascontiguousarray(np.array(maps, dtype=np.uint8)), game


class Opponents:
    """the opponent mix: scripted bots and frozen actors"""
    BOTS = (D.BOT_HUNTER, D.BOT_SITTER, D.BOT_WANDER)

    def __init__(self, bot_frac=(0.15, 0.1, 0.1)):
        self.bot_frac = bot_frac
        self.models = []     # (name, actor)

    def set_models(self, models):
        self.models = models

    def draw(self, rng, n):
        """controller ids: -1..-3 bots (hunter, sitter, wander), k >= 0 = models[k]"""
        p = list(self.bot_frac)
        if not self.models:
            p = [x / sum(p) for x in p]
            return -1 - rng.choice(3, size=n, p=p)
        rest = 1 - sum(p)
        nm = len(self.models)
        # half of the model share to the newest quarter of the pool
        new = max(1, nm // 4)
        pm = np.full(nm, rest * 0.5 / nm)
        pm[-new:] += rest * 0.5 / new
        probs = np.concatenate([p, pm])
        probs /= probs.sum()
        c = rng.choice(3 + nm, size=n, p=probs)
        return np.where(c < 3, -1 - c, c - 3)


class DuelEnv:
    def __init__(self, n, agent_type, seed=0, nt=3, opp_type=D.T_PLAYER, random_frac=0.5, opponents=None,
                 ally_frac=0.4):
        self.n, self.nt = n, nt
        self.agent_type, self.opp_type = agent_type, opp_type
        self.rng = np.random.default_rng(seed)
        self.maps, self.ngame = arena_pool(self.rng)
        self.labels = [components(g) for g in self.maps]
        self.frees = [np.argwhere(g == 0) for g in self.maps]
        self.random_frac = random_frac
        self.ally_frac = ally_frac
        self.bot_fire_p = 1.0          # curriculum: scripted bots fire only with this probability
        nmax = D.TYPES[agent_type][8]
        ns = nmax + D.TYPES[opp_type][8] + (nmax if nt > 2 else 0)
        self.tw = Twin(n, nt, ns=ns)
        self.style = STYLE[agent_type]
        self.opp = opponents or Opponents()
        self.ctrl = np.zeros((n, nt), dtype=np.int64)      # per tank (1..): controller id
        self.ep_ret = np.zeros(n)
        self.ep_len = np.zeros(n, dtype=np.int64)
        self.prev_dist = np.zeros(n)
        self.stats = []
        self.reset(np.ones(n, bool))

    # ------------------------------------------------------------------ resets
    def reset(self, m):
        idx = np.nonzero(m)[0]
        if not len(idx):
            return
        tw, rng = self.tw, self.rng
        tw.init(0, rng.integers(1, 2 ** 31, size=self.n), mask=m)
        nt = self.nt
        full = {f: np.zeros((self.n, nt), dtype=np.int32) for f in ("typ", "team", "x", "y", "a")}
        for e in idx:
            for _ in range(50):
                if rng.random() < self.random_frac:
                    mi = rng.integers(self.ngame, len(self.maps))
                else:
                    mi = rng.integers(self.ngame)
                g = self.maps[mi]
                free = self.frees[mi]            # (y, x)
                pts = free[rng.choice(len(free), size=nt, replace=False)]
                a, b = pts[0], pts[1]
                if max(abs(a[0] - b[0]), abs(a[1] - b[1])) < 6:
                    continue
                if nt > 2 and max(abs(pts[2][0] - a[0]), abs(pts[2][1] - a[1])) < 2:
                    continue
                lab = self.labels[mi]
                if lab[a[0], a[1]] != lab[b[0], b[1]]:
                    continue
                break
            tw.map[e] = g
            for i in range(nt):
                y, x = pts[i]
                full["typ"][e, i] = self.opp_type if i == 1 else self.agent_type
                full["team"][e, i] = 1 if i == 1 else 0
                full["x"][e, i] = x * 256 + 128
                full["y"][e, i] = y * 256 + 128
                full["a"][e, i] = rng.integers(256)
        tw.sync_map(m)
        for i in range(nt):
            tw.set_tank(i, full["typ"][:, i], full["team"][:, i], full["x"][:, i], full["y"][:, i], full["a"][:, i],
                        mask=m)
        if nt > 2:      # the ally is there in ally_frac of the games
            gone = m & (rng.random(self.n) >= self.ally_frac)
            tw.t["alive"][gone, 2] = 0
        c = self.opp.draw(rng, len(idx))
        self.ctrl[idx, 1] = c
        if nt > 2:
            self.ctrl[idx, 2] = self.opp.draw(rng, len(idx))
        self.ep_ret[idx] = 0
        self.ep_len[idx] = 0
        self.prev_dist[idx] = self.dist()[idx]

    def dist(self):
        t = self.tw.t
        dx = np.abs(t["x"][:, 1] - t["x"][:, 0]) >> 4
        dy = np.abs(t["y"][:, 1] - t["y"][:, 0]) >> 4
        return np.maximum(dx, dy).astype(np.float64)

    # ------------------------------------------------------------------ control of the other tanks
    def drive_other(self, i):
        tw = self.tw
        c = self.ctrl[:, i]
        for k, kind in enumerate(Opponents.BOTS):
            mk = c == -1 - k
            if mk.any():
                tw.bot_act(i, kind, mk)
                if self.bot_fire_p < 1:
                    hold = mk & (self.rng.random(self.n) >= self.bot_fire_p)
                    tw.t["fire"][hold, i] = 0
        if (c >= 0).any():
            o = tw.obs(i)
            for k in np.unique(c[c >= 0]):
                mk = c == k
                with torch.no_grad():
                    logits = self.opp.models[k][1](torch.from_numpy(o[mk]))
                    if not self.greedy_opp:          # Gumbel-max: a sample from each head's softmax
                        logits = logits - torch.log(-torch.log(torch.rand_like(logits).clamp_(1e-9, 1.0)))
                acts = torch.stack([lg.argmax(-1) for lg in torch.split(logits, (3, 3, 7, 2), -1)], -1).numpy()
                full = np.zeros((self.n, 4), dtype=np.int32)
                full[mk] = acts
                tw.apply_action(i, full, mk)

    greedy_opp = False

    # ------------------------------------------------------------------ step
    def obs(self):
        return self.tw.obs(0)

    def step(self, act, agent_obs):
        tw, st = self.tw, self.style
        tw.apply_action(0, act.astype(np.int32))
        shots0 = tw.t["nshells"][:, 0].copy()
        cool0 = tw.t["cool"][:, 0].copy()
        for k in range(D.DECIDE):
            for i in range(1, self.nt):
                if k == i % D.DECIDE:
                    self.drive_other(i)
            tw.step()
        t = tw.t
        a_dead = t["alive"][:, 0] == 0
        o_dead = t["alive"][:, 1] == 0
        r = np.zeros(self.n)
        r += np.where(o_dead & ~a_dead, 1.0, 0.0)
        r += np.where(a_dead & ~o_dead, -1.0, 0.0)
        r += np.where(a_dead & o_dead, -0.5, 0.0)
        if st["bank"]:
            r += np.where(o_dead & (t["by"][:, 1] == 0) & tw.kill_bounce[:, 1], st["bank"], 0.0)
        # style terms
        d = self.dist()
        if st["approach"]:
            r += st["approach"] * (self.prev_dist - d)
        self.prev_dist = d
        if st["far_pen"]:
            r -= st["far_pen"] * (d > st["far"])
        if st["near_pen"]:
            r -= st["near_pen"] * (d < st["near"])
        if st["exposed"]:
            o = agent_obs
            exposed = (o[:, 21] == 127) & (o[:, 24] >= 110) & (o[:, 25] == 127)
            r -= st["exposed"] * exposed
        if st["shot"]:
            fired = (t["cool"][:, 0] > cool0) | (t["nshells"][:, 0] > shots0)
            r -= st["shot"] * fired
        done = a_dead | o_dead
        timeout = ~done & (tw.tick >= EP_TICKS)
        r += np.where(timeout, st["timeout"], 0.0)
        done |= timeout
        self.ep_ret += r
        self.ep_len += 1
        if done.any():
            for e in np.nonzero(done)[0]:
                res = 1 if (o_dead[e] and not a_dead[e]) else (-1 if a_dead[e] and not o_dead[e] else 0)
                self.stats.append((int(self.ctrl[e, 1]), res, int(self.ep_len[e]), float(self.ep_ret[e]),
                                   bool(timeout[e])))
            self.reset(done)
        return r, done

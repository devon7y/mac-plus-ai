"""Many DuelEnv shards in worker processes (the numpy twin is single-threaded, so this is how
training uses a whole node). Same interface as DuelEnv for ppo.py: obs(), step(act, obs),
stats, bot_fire_p, and set_pool(list of (name, actor)) for the league opponents."""
import multiprocessing as mp

import numpy as np


def _worker(conn, n, agent_type, seed, kw):
    import torch
    torch.set_num_threads(1)
    from env import DuelEnv, Opponents
    from model import Actor
    opp = Opponents()
    env = DuelEnv(n, agent_type, seed=seed, opponents=opp, **kw)
    obs = env.obs()
    while True:
        try:
            cmd, arg = conn.recv()
        except EOFError:
            break
        if cmd == "obs":
            conn.send(obs)
        elif cmd == "step":
            env.stats = []
            r, done = env.step(arg, obs)
            obs = env.obs()
            conn.send((obs, r, done, env.stats))
        elif cmd == "botfire":
            env.bot_fire_p = arg
        elif cmd == "pool":
            models = []
            for name, sd, quant, hidden in arg:
                a = Actor(hidden=hidden)
                a.load_state_dict(sd)
                a.quant = quant
                a.eval()
                models.append((name, a))
            opp.set_models(models)
        elif cmd == "close":
            break


class VecDuel:
    def __init__(self, n, agent_type, workers, seed=0, **kw):
        ctx = mp.get_context("spawn")
        self.k = workers
        sizes = [n // workers + (1 if i < n % workers else 0) for i in range(workers)]
        self.bounds = np.cumsum([0] + sizes)
        self.conns, self.procs = [], []
        for i in range(workers):
            a, b = ctx.Pipe()
            p = ctx.Process(target=_worker, args=(b, sizes[i], agent_type, seed * 1000 + i, kw), daemon=True)
            p.start()
            self.conns.append(a)
            self.procs.append(p)
        self.stats = []
        self._bot_fire_p = 1.0

    @property
    def bot_fire_p(self):
        return self._bot_fire_p

    @bot_fire_p.setter
    def bot_fire_p(self, v):
        if v != self._bot_fire_p:
            self._bot_fire_p = v
            for c in self.conns:
                c.send(("botfire", v))

    def set_pool(self, models):
        msg = [(name, {k: t.clone() for k, t in a.state_dict().items()}, a.quant, a.hidden) for name, a in models]
        for c in self.conns:
            c.send(("pool", msg))

    def obs(self):
        for c in self.conns:
            c.send(("obs", None))
        return np.concatenate([c.recv() for c in self.conns])

    def step(self, act, obs=None):
        for i, c in enumerate(self.conns):
            c.send(("step", act[self.bounds[i]:self.bounds[i + 1]]))
        outs = [c.recv() for c in self.conns]
        self._obs = np.concatenate([o[0] for o in outs])
        r = np.concatenate([o[1] for o in outs])
        done = np.concatenate([o[2] for o in outs])
        for o in outs:
            self.stats.extend(o[3])
        return r, done

    def last_obs(self):
        return self._obs

    def close(self):
        for c in self.conns:
            c.send(("close", None))

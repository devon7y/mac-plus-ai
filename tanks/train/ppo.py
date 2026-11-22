#!/usr/bin/env python3
"""PPO for one Tank Duel tank type, against scripted bots and a league pool of frozen policies.

    ppo.py --type rusher --out runs/rusher --pool pool [--steps 150e6] [--n 2048] [--hidden 32]
           [--quant_at 0.6] [--threads 8] [--max_hours 6]

Every --snap updates the actor is saved into the pool folder (shared by the runs of all types),
and the pool is re-read every --reload updates, so the types train against each other's past
versions. From --quant_at (fraction of --steps) on, the actor trains with ternary weights and
quantized activations, so the final checkpoint is exactly the integer net the Mac runs
(export: model.export_int).
"""
import argparse
import glob
import json
import os
import sys
import time

import numpy as np
import torch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "sim"))
import defs as D  # noqa: E402
from env import DuelEnv, Opponents  # noqa: E402
from model import Actor, Critic, sample, evaluate, export_int, save_int  # noqa: E402

TYPES = {"player": D.T_PLAYER, "rusher": D.T_RUSHER, "sniper": D.T_SNIPER, "careful": D.T_CAREFUL}


def load_actor(path):
    ck = torch.load(path, map_location="cpu", weights_only=False)
    a = Actor(hidden=ck["hidden"])
    a.load_state_dict(ck["actor"])
    a.quant = ck["quant"]
    a.eval()
    return a


def read_pool(pool, rng, max_models=14):
    files = sorted(glob.glob(os.path.join(pool, "*.pt")), key=os.path.getmtime)
    if not files:
        return []
    keep = files[-8:]
    older = files[:-8]
    if older:
        keep = list(rng.choice(older, size=min(len(older), max_models - len(keep)), replace=False)) + keep
    out = []
    for f in keep:
        try:
            out.append((os.path.basename(f), load_actor(f)))
        except Exception as e:      # a file being written
            print("pool: skip", f, e, flush=True)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--type", required=True, choices=list(TYPES))
    ap.add_argument("--out", required=True)
    ap.add_argument("--pool", required=True)
    ap.add_argument("--steps", type=float, default=150e6)
    ap.add_argument("--n", type=int, default=2048)
    ap.add_argument("--T", type=int, default=32)
    ap.add_argument("--hidden", type=int, default=32)
    ap.add_argument("--quant_at", type=float, default=0.6)
    ap.add_argument("--lr", type=float, default=3e-4)
    ap.add_argument("--ent", type=float, default=0.01)
    ap.add_argument("--epochs", type=int, default=4)
    ap.add_argument("--mb", type=int, default=8)
    ap.add_argument("--gamma", type=float, default=0.99)
    ap.add_argument("--lam", type=float, default=0.95)
    ap.add_argument("--snap", type=int, default=50)
    ap.add_argument("--reload", type=int, default=25)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--max_hours", type=float, default=6)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--resume", default="")
    ap.add_argument("--workers", type=int, default=1, help="processes running shards of the games")
    ap.add_argument("--handicap", type=float, default=0.15, help="bots fire less until this fraction of --steps")
    a = ap.parse_args()
    torch.set_num_threads(a.threads)
    torch.manual_seed(a.seed)
    os.makedirs(a.out, exist_ok=True)
    os.makedirs(a.pool, exist_ok=True)
    rng = np.random.default_rng(a.seed + 1000)
    typ = TYPES[a.type]

    actor, critic = Actor(hidden=a.hidden), Critic()
    opt = torch.optim.Adam(list(actor.parameters()) + list(critic.parameters()), lr=a.lr, eps=1e-5)
    step0 = 0
    if a.resume:
        ck = torch.load(a.resume, map_location="cpu", weights_only=False)
        actor.load_state_dict(ck["actor"])
        critic.load_state_dict(ck["critic"])
        opt.load_state_dict(ck["opt"])
        step0 = ck["steps"]
        actor.quant = ck["quant"]

    opps = Opponents()
    if a.workers > 1:
        from vec import VecDuel
        env = VecDuel(a.n, typ, a.workers, seed=a.seed)
    else:
        env = DuelEnv(a.n, typ, seed=a.seed, opponents=opps)
    obs = env.obs()
    N, T = a.n, a.T
    per_update = N * T
    total = int(a.steps)
    log = open(os.path.join(a.out, "log.jsonl"), "a")
    t_start = time.time()
    steps = step0
    upd = 0

    def save(path, extra=None):
        d = dict(actor=actor.state_dict(), critic=critic.state_dict(), opt=opt.state_dict(), steps=steps,
                 quant=actor.quant, hidden=a.hidden, type=a.type)
        if extra:
            d.update(extra)
        tmp = path + ".tmp"
        torch.save(d, tmp)
        os.replace(tmp, path)

    while steps < total:
        # progress: by steps or by time, whichever is further (so QAT and the LR decay always happen)
        frac = max(steps / total, (time.time() - t_start) / 3600 / (a.max_hours * 0.97))
        if not actor.quant and frac >= a.quant_at:
            actor.quant = True
            print(f"step {steps}: quantization on", flush=True)
        for g in opt.param_groups:
            g["lr"] = a.lr * (1 - 0.7 * frac)
        ent_coef = a.ent * (1 - 0.8 * frac)
        env.bot_fire_p = min(1.0, 0.25 + 0.75 * frac / a.handicap) if a.handicap > 0 else 1.0
        if upd % a.reload == 0:
            pool = read_pool(a.pool, rng)
            if a.workers > 1:
                env.set_pool(pool)
            else:
                opps.set_models(pool)
        # ---------------------------------------------------------- rollout
        b_obs = np.zeros((T, N, D.NOBS), dtype=np.int16)
        b_act = np.zeros((T, N, 4), dtype=np.int64)
        b_logp = np.zeros((T, N), dtype=np.float32)
        b_val = np.zeros((T + 1, N), dtype=np.float32)
        b_rew = np.zeros((T, N), dtype=np.float32)
        b_done = np.zeros((T, N), dtype=np.float32)
        t0 = time.time()
        env.stats = []
        for t in range(T):
            ot = torch.from_numpy(obs.astype(np.float32))
            with torch.no_grad():
                logits = actor(ot)
                act, logp, _ = sample(logits)
                val = critic(ot)
            b_obs[t] = obs
            b_act[t] = act.numpy()
            b_logp[t] = logp.numpy()
            b_val[t] = val.numpy()
            r, done = env.step(act.numpy(), obs)
            b_rew[t] = r
            b_done[t] = done
            obs = env.last_obs() if a.workers > 1 else env.obs()
        with torch.no_grad():
            b_val[T] = critic(torch.from_numpy(obs.astype(np.float32))).numpy()
        t_roll = time.time() - t0
        # ---------------------------------------------------------- GAE
        adv = np.zeros((T, N), dtype=np.float32)
        last = np.zeros(N, dtype=np.float32)
        for t in reversed(range(T)):
            nonterm = 1.0 - b_done[t]
            delta = b_rew[t] + a.gamma * b_val[t + 1] * nonterm - b_val[t]
            last = delta + a.gamma * a.lam * nonterm * last
            adv[t] = last
        ret = adv + b_val[:T]
        # ---------------------------------------------------------- update
        fo = torch.from_numpy(b_obs.reshape(-1, D.NOBS).astype(np.float32))
        fa = torch.from_numpy(b_act.reshape(-1, 4))
        flp = torch.from_numpy(b_logp.reshape(-1))
        fadv = torch.from_numpy(adv.reshape(-1))
        fret = torch.from_numpy(ret.reshape(-1))
        n_all = fo.shape[0]
        mbs = n_all // a.mb
        kl_sum, pl_sum, vl_sum, ent_sum, nb = 0, 0, 0, 0, 0
        for ep in range(a.epochs):
            perm = torch.randperm(n_all)
            for k in range(a.mb):
                ix = perm[k * mbs:(k + 1) * mbs]
                logits = actor(fo[ix])
                lp, ent = evaluate(logits, fa[ix])
                ratio = torch.exp(lp - flp[ix])
                ad = fadv[ix]
                ad = (ad - ad.mean()) / (ad.std() + 1e-8)
                pl = -torch.min(ratio * ad, torch.clamp(ratio, 0.8, 1.2) * ad).mean()
                v = critic(fo[ix])
                vl = 0.5 * ((v - fret[ix]) ** 2).mean()
                loss = pl + 0.5 * vl - ent_coef * ent.mean()
                opt.zero_grad()
                loss.backward()
                torch.nn.utils.clip_grad_norm_(list(actor.parameters()) + list(critic.parameters()), 0.5)
                opt.step()
                with torch.no_grad():
                    kl_sum += (flp[ix] - lp).mean().item()
                pl_sum += pl.item()
                vl_sum += vl.item()
                ent_sum += ent.mean().item()
                nb += 1
        steps += per_update
        upd += 1
        # ---------------------------------------------------------- log
        st = env.stats
        def rate(sel):
            s = [x for x in st if sel(x[0])]
            if not s:
                return None
            return round(sum(1 for x in s if x[1] == 1) / len(s), 3), round(sum(1 for x in s if x[1] == -1) / len(s), 3), len(s)
        rec = dict(steps=steps, upd=upd, sps=round(per_update / (time.time() - t0)), roll=round(t_roll, 2),
                   quant=actor.quant, kl=round(kl_sum / nb, 4), pl=round(pl_sum / nb, 4), vl=round(vl_sum / nb, 4),
                   ent=round(ent_sum / nb, 3), eps=len(st),
                   eplen=round(float(np.mean([x[2] for x in st])), 1) if st else None,
                   ret=round(float(np.mean([x[3] for x in st])), 3) if st else None,
                   hunter=rate(lambda c: c == -1), sitter=rate(lambda c: c == -2), wander=rate(lambda c: c == -3),
                   pool=rate(lambda c: c >= 0), botfire=round(env.bot_fire_p, 2), hours=round((time.time() - t_start) / 3600, 3))
        log.write(json.dumps(rec) + "\n")
        log.flush()
        if upd % 10 == 0:
            print(json.dumps(rec), flush=True)
        if upd % a.snap == 0:
            save(os.path.join(a.pool, f"{a.type}_{steps // 1000000:04d}M.pt"))
            save(os.path.join(a.out, "latest.pt"))
            save(os.path.join(a.out, f"ckpt_{steps // 1000000:04d}M.pt"))
        if (time.time() - t_start) / 3600 > a.max_hours:
            print("time limit", flush=True)
            break
    if a.workers > 1:
        env.close()
    save(os.path.join(a.out, "latest.pt"))
    save(os.path.join(a.out, "final.pt"))
    save_int(export_int(actor), os.path.join(a.out, "final_int.npz"))
    print("done", steps, flush=True)


if __name__ == "__main__":
    main()

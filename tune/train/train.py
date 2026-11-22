"""Train Tune's GRU on tune/data/prep.npz.

  python3 -I tune/train/train.py --H 256 --epochs 40 --qat-from 20 --out tune/build/ckpt/h256.pt
"""
import argparse
import math
import os
import sys
import time

import numpy as np
import torch
import torch.nn.functional as F

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from model import TuneNet, FEATS  # noqa: E402
NF = len(FEATS)
import vocab as V  # noqa: E402

DATA = os.path.join(HERE, "..", "data")


def load():
    z = np.load(os.path.join(DATA, "prep.npz"))
    feats, tgt, cond, off, split = z["feats"], z["tgt"], z["cond"], z["off"], z["split"]
    mask = np.unpackbits(z["mask"], axis=1)[:, :V.VOCAB].astype(bool)
    tunes = []
    for i in range(len(off) - 1):
        a, b = off[i], off[i + 1]
        tunes.append((feats[a:b], tgt[a:b], mask[a:b], cond[i], split[i]))
    return tunes


def batches(tunes, bs, shuffle, rng):
    idx = sorted(range(len(tunes)), key=lambda i: len(tunes[i][1]))
    groups = [idx[i:i + bs] for i in range(0, len(idx), bs)]
    if shuffle:
        rng.shuffle(groups)
    for g in groups:
        T = max(len(tunes[i][1]) for i in g)
        B = len(g)
        f = np.zeros((B, T, NF), np.int64)
        y = np.full((B, T), -100, np.int64)
        m = np.ones((B, T, V.VOCAB), bool)
        c = np.zeros((B, 3), np.int64)
        for j, i in enumerate(g):
            ff, tt, mm, cc, _ = tunes[i]
            n = len(tt)
            f[j, :n] = ff
            y[j, :n] = tt
            m[j, :n] = mm
            c[j] = cc
        yield torch.from_numpy(f), torch.from_numpy(c), torch.from_numpy(m), torch.from_numpy(y)


def evaluate(net, tunes, bs=64):
    net.eval()
    tot, n = 0.0, 0
    with torch.no_grad():
        for f, c, m, y in batches(tunes, bs, False, None):
            lg = net(f, c, m)
            loss = F.cross_entropy(lg.reshape(-1, V.VOCAB), y.reshape(-1), ignore_index=-100, reduction="sum")
            tot += loss.item()
            n += (y >= 0).sum().item()
    net.train()
    return tot / n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--H", type=int, default=256)
    ap.add_argument("--layers", type=int, default=1)
    ap.add_argument("--H2", type=int, default=0)
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--qat-from", type=int, default=-1)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--bs", type=int, default=32)
    ap.add_argument("--drop", type=float, default=0.1)
    ap.add_argument("--wd", type=float, default=0.01)
    ap.add_argument("--threads", type=int, default=3)
    ap.add_argument("--init", default="")
    ap.add_argument("--nolookback", action="store_true")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    torch.set_num_threads(a.threads)
    torch.manual_seed(1)
    rng = np.random.default_rng(1)
    tunes = load()
    if a.nolookback:        # zero the look-back inputs (ablation): "none" everywhere
        tunes = [(np.concatenate([f[:, :6], np.tile([V.NSYM, 0, V.NSYM, 0], (len(f), 1))], 1).astype(f.dtype),) + t[1:]
                 for t in tunes for f in [t[0]]]
    train = [t for t in tunes if t[4] == 0]
    val = [t for t in tunes if t[4] == 1]
    net = TuneNet(a.H, a.layers, a.H2 or None, a.drop)
    if a.init:
        net.load_state_dict(torch.load(a.init)["model"])
    print("params", sum(p.numel() for p in net.parameters()), "mac weights/token", net.macs(), flush=True)
    opt = torch.optim.AdamW(net.parameters(), lr=a.lr, weight_decay=a.wd)
    nb = math.ceil(len(train) / a.bs)
    total = a.epochs * nb
    step = 0
    best = 1e9
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    for ep in range(a.epochs):
        net.qat = a.qat_from >= 0 and ep >= a.qat_from
        if a.qat_from >= 0 and ep == a.qat_from:
            best = 1e9          # judge QAT checkpoints among themselves
        t0 = time.time()
        tl, tn = 0.0, 0
        for f, c, m, y in batches(train, a.bs, True, rng):
            lr = a.lr * 0.5 * (1 + math.cos(math.pi * step / total))
            for g in opt.param_groups:
                g["lr"] = lr
            lg = net(f, c, m)
            loss = F.cross_entropy(lg.reshape(-1, V.VOCAB), y.reshape(-1), ignore_index=-100)
            opt.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(net.parameters(), 1.0)
            opt.step()
            k = (y >= 0).sum().item()
            tl += loss.item() * k
            tn += k
            step += 1
        vl = evaluate(net, val)
        mark = ""
        if vl < best:
            best = vl
            torch.save({"model": net.state_dict(), "args": vars(a), "val": vl, "epoch": ep, "qat": net.qat}, a.out)
            mark = " *"
        print("ep %d train %.4f val %.4f qat %d %.0fs%s" % (ep, tl / tn, vl, net.qat, time.time() - t0, mark), flush=True)


if __name__ == "__main__":
    main()

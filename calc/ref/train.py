#!/usr/bin/env python3
"""Train Scribble Calc's classifier: float pre-training, then ternary QAT, then export.

    python3 -I train.py --spec S --epochs 10 --qat-epochs 10 --out ../build/models/S.pt
    python3 -I train.py --compare S S2 T ...      (float only, a few epochs each: M0 table)
"""
import argparse
import os
import sys
import time

import numpy as np
import torch
import torch.nn.functional as F

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from model import SPECS, Net, cost, params      # noqa: E402
from strokes import CALC, NCLS                  # noqa: E402

DATA = os.path.join(CALC, "build", "data")
TESTS = ["pd_clean", "pd_mouse", "uji_clean", "uji_mouse", "syn_mouse"]
dev = torch.device("mps" if torch.backends.mps.is_available() else "cpu")


def load(name):
    d = np.load(os.path.join(DATA, name + ".npz"))
    X = np.unpackbits(d["X"], axis=1).reshape(-1, 1, 24, 24).astype(np.float32)
    return torch.from_numpy(X), torch.from_numpy(d["Y"].astype(np.int64))


def evaluate(net, sets, bs=4096):
    net.eval()
    res = {}
    with torch.no_grad():
        for k, (X, Y) in sets.items():
            ok = 0
            for i in range(0, len(X), bs):
                p = net(X[i:i + bs].to(dev)).argmax(1).cpu()
                ok += (p == Y[i:i + bs]).sum().item()
            res[k] = ok / len(X)
    return res


def run_epochs(net, X, Y, epochs, lr, sets, tag, bs=256, wd=1e-4):
    opt = torch.optim.AdamW(net.parameters(), lr=lr, weight_decay=wd)
    steps = epochs * ((len(X) + bs - 1) // bs)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, lr, total_steps=steps, pct_start=0.15)
    g = torch.Generator().manual_seed(0)
    for ep in range(epochs):
        net.train()
        perm = torch.randperm(len(X), generator=g)
        t0, tl, n = time.time(), 0.0, 0
        for i in range(0, len(X), bs):
            idx = perm[i:i + bs]
            xb, yb = X[idx].to(dev), Y[idx].to(dev)
            # light on-the-fly augmentation: random 1-px shift
            if torch.rand(1).item() < 0.5:
                dx, dy = np.random.randint(-1, 2, 2)
                xb = torch.roll(xb, shifts=(int(dy), int(dx)), dims=(2, 3))
            loss = F.cross_entropy(net(xb), yb, label_smoothing=0.05)
            opt.zero_grad()
            loss.backward()
            opt.step()
            sched.step()
            tl += loss.item() * len(idx)
            n += len(idx)
        r = evaluate(net, sets)
        print(f"[{tag}] ep {ep + 1}/{epochs} loss {tl / n:.4f} " + " ".join(f"{k} {v * 100:.2f}" for k, v in r.items())
              + f" ({time.time() - t0:.0f}s)", flush=True)
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--spec", default="S")
    ap.add_argument("--compare", nargs="*")
    ap.add_argument("--epochs", type=int, default=10)
    ap.add_argument("--qat-epochs", type=int, default=10)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--out")
    ap.add_argument("--seed", type=int, default=0)
    a = ap.parse_args()
    torch.manual_seed(a.seed)
    np.random.seed(a.seed)
    X, Y = load("train")
    sets = {k: load("test_" + k) for k in TESTS}
    if a.compare:
        for name in a.compare:
            spec = SPECS[name]
            net = Net(spec).to(dev)
            r = run_epochs(net, X, Y, a.epochs, a.lr, sets, name)
            c, code, _ = cost(spec)
            print(f"RESULT {name} params {params(spec)} est_cycles {c} realPlus_s {c / 5.14e6:.3f} code {code} "
                  + " ".join(f"{k} {v * 100:.2f}" for k, v in r.items()), flush=True)
        return
    spec = SPECS[a.spec]
    net = Net(spec).to(dev)
    run_epochs(net, X, Y, a.epochs, a.lr, sets, a.spec + " float")
    # QAT: calibrate activation scales on a training subset, then train with ternary weights
    net.calibrate(X[:20000].to(dev))
    net.qat = True
    r = evaluate(net, sets)
    print("after calibration (QAT forward, before fine-tuning): " + " ".join(f"{k} {v * 100:.2f}" for k, v in r.items()))
    run_epochs(net, X, Y, a.qat_epochs, a.lr / 3, sets, a.spec + " qat")
    if a.out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        torch.save({"spec": spec, "state": net.state_dict()}, a.out)
        print("saved", a.out)


if __name__ == "__main__":
    main()

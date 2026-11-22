#!/usr/bin/env python3
"""Architecture spec (train/gnet.py) -> the tensors and layers of a model file, and random models
for the benchmark.

    layout.py SPEC NCLS OUT.gmdl [seed]     writes a random model (realistic sparsity and scales)
"""
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "train"))
import gnet  # noqa: E402

import model_file  # noqa: E402


def layout(spec, ncls, G=32):
    """Returns (tensors [H, W, C, pad], layers [dict]) without weights. A per layer comes from
    gnet.Net's rule (the next weighted layer's fan-in)."""
    sh = gnet.shapes(spec, ncls, G)
    net_A = []
    kinds = [s[0][0] for s in sh]
    fans = [s[3] for s in sh]
    for j in range(len(sh)):
        nxt = next((fans[q] for q in range(j + 1, len(sh)) if kinds[q] in "dclo"), 1)
        net_A.append(min(gnet.AMAX, 32767 // max(nxt, 1)))
    tensors = [[G, G, 1, 0]]
    layers = []
    cur = 0
    for j, (L, i, o, fan, par, mac) in enumerate(sh):
        k = L[0]
        if k == "f":
            continue
        H, W, C = o
        tensors.append([H, W, C, 0])
        dst = len(tensors) - 1
        if k == "t":
            l = dict(kind="T", cout=C, cin=1, k=3, stride=L[2], A=net_A[j])
        elif k == "p":
            l = dict(kind="P", cin=C, cout=C)
        elif k == "g":
            l = dict(kind="G", cin=C, cout=C)
        elif k == "d":
            l = dict(kind="D", cin=C, cout=C, k=3, A=net_A[j])
        elif k == "c":
            l = dict(kind="C", cin=i[2], cout=C, k=L[2], stride=L[3], A=net_A[j])
        elif k == "l":
            l = dict(kind="L", cin=fan, cout=C, A=net_A[j])
        elif k == "o":
            l = dict(kind="L", cin=fan, cout=C, A=-1)
        l["src"], l["dst"] = cur, dst
        layers.append(l)
        cur = dst
    for l in layers:                      # the input of a 3x3 layer gets a zero border
        if l["kind"] == "D" or (l["kind"] == "C" and l["k"] == 3):
            tensors[l["src"]][3] = 1
    return [tuple(t) for t in tensors], layers


def random_model(spec, ncls, seed=1, rho=0.55):
    rng = np.random.RandomState(seed)
    tensors, layers = layout(spec, ncls)
    for l in layers:
        k = l["kind"]
        if k == "T":
            A = l["A"]
            w = rng.randn(l["cout"], 9)
            b = rng.randn(l["cout"]) - 0.5
            pats = (np.arange(512)[:, None] >> (8 - np.arange(9))[None]) & 1
            pre = pats @ w.T + b
            l["table"] = np.clip(np.round(pre * A / 4), 0, A).astype(np.int64)
        elif k in ("D", "C", "L"):
            n = model_file.nweights(l)
            u = rng.rand(n)
            l["w"] = np.where(u < rho, np.where(rng.rand(n) < 0.5, 1, -1), 0).astype(np.int8)
            fan = n // l["cout"]
            g = 1.0 / math.sqrt(rho * fan) * (1.0 if l["A"] < 0 else 1.2)
            S = np.full(l["cout"], 22)
            M = np.round(g * (2.0 ** S) * np.exp(rng.randn(l["cout"]) * 0.2)).astype(np.int64)
            while (np.abs(M) > 32767).any():
                big = np.abs(M) > 32767
                M[big] //= 2
                S[big] -= 1
            amp = max(l["A"], 32) / 4
            B = np.round((rng.randn(l["cout"]) * amp) * (2.0 ** S)).astype(np.int64)
            l["M"], l["S"], l["B"] = M, S, B
    names = [f"class{i}" for i in range(ncls)]
    return dict(G=32, tensors=tensors, layers=layers, names=names)


if __name__ == "__main__":
    spec, ncls, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    seed = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    m = random_model(spec, ncls, seed)
    n = model_file.write(out, m)
    print(f"{out}: {n} bytes, {len(m['layers'])} layers, tensors {m['tensors']}")

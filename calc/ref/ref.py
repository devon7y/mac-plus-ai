#!/usr/bin/env python3
"""Scribble Calc: export a trained model to integers, and the bit-exact integer reference.

    python3 -I ref.py export ../build/models/S2.pt ../build/models/S2.npz
    python3 -I ref.py eval ../build/models/S2.npz            (accuracy of the integer model)

Integer model (what the Mac runs; app/net.c + the generated net68k.s):
  layer "lut": T[512][C] uint8 (0..127); for each 3x3 window of the zero-padded 24x24 input,
      idx = sum(bit(y+dy-1, x+dx-1) << (8 - 3*dy - dx)); conv value = T[idx][c]; then 2x2 max pool.
  layer "conv": t[co][dy][dx][ci] in {-1,0,1}; input a (H, W, Ci) zero-padded by 1;
      acc[y][x][co] = sum t * a[s*y+dy][s*x+dx][ci] (int16; never overflows: checked);
      optional 2x2 max pool of acc; q = clamp(((acc + b) * m + 32768) >> 16, 0, amax).
  layer "fc": t[o][i] with i over the (H, W, C) flattened input; same requant; the last fc gives
      scores s = ((acc + b) * m + 32768) >> 16 (no clamp), in 1/8 nat units.
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from strokes import G   # noqa: E402

SCORE_UNIT = 8.0         # scores are logits * 8 (1/8 nat)


def lut_index_image(img):
    """img (24, 24) 0/1 -> idx (24, 24) of 9-bit window codes."""
    p = np.zeros((G + 2, G + 2), np.int64)
    p[1:-1, 1:-1] = img
    idx = np.zeros((G, G), np.int64)
    for dy in range(3):
        for dx in range(3):
            idx |= p[dy:dy + G, dx:dx + G] << (8 - 3 * dy - dx)
    return idx


def requant(acc, b, m, amax):
    v = ((acc.astype(np.int64) + b) * m + 32768) >> 16
    return v if amax is None else np.clip(v, 0, amax)


def forward(M, img, trace=None):
    """Integer forward pass of one raster (24, 24) -> int scores (18,)."""
    L = M["layers"]
    idx = lut_index_image(img)
    T = M["lut"]                                   # (512, C)
    conv = T[idx]                                  # (24, 24, C)
    H = G // 2
    a = conv.reshape(H, 2, H, 2, -1).max(axis=(1, 3)).astype(np.int64)
    if trace is not None:
        trace.append(a.copy())
    for k, l in enumerate(L[1:], 1):
        t, b, m = M[f"t{k}"], M[f"b{k}"], M[f"m{k}"]
        last = k == len(L) - 1
        if l["kind"] == "conv":
            Hi = a.shape[0]
            Ci = a.shape[2]
            s = l["stride"]
            p = np.zeros((Hi + 2, Hi + 2, Ci), np.int64)
            p[1:-1, 1:-1] = a
            Hc = (Hi + s - 1) // s
            acc = np.zeros((Hc, Hc, t.shape[0]), np.int64)
            for dy in range(3):
                for dx in range(3):
                    win = p[dy:dy + s * Hc:s, dx:dx + s * Hc:s, :]       # (Hc, Hc, Ci)
                    acc += np.einsum("yxc,oc->yxo", win, t[:, dy, dx, :])
            assert np.abs(acc).max() < 32768, "int16 overflow"
            if l["pool"]:
                acc = acc.reshape(Hc // 2, 2, Hc // 2, 2, -1).max(axis=(1, 3))
            a = requant(acc, b, m, l["amax"])
        else:
            x = a.reshape(-1)
            acc = t @ x
            assert np.abs(acc).max() < 32768, "int16 overflow"
            a = requant(acc, b, m, None if last else l["amax"])
        if trace is not None:
            trace.append(a.copy())
    return a


def forward_batch(M, imgs):
    """Vectorised integer forward pass for many rasters (N, 24, 24) -> (N, 18). Same arithmetic."""
    L = M["layers"]
    N = len(imgs)
    p = np.zeros((N, G + 2, G + 2), np.int64)
    p[:, 1:-1, 1:-1] = imgs
    idx = np.zeros((N, G, G), np.int64)
    for dy in range(3):
        for dx in range(3):
            idx |= p[:, dy:dy + G, dx:dx + G] << (8 - 3 * dy - dx)
    conv = M["lut"][idx]
    H = G // 2
    a = conv.reshape(N, H, 2, H, 2, -1).max(axis=(2, 4)).astype(np.int64)
    for k, l in enumerate(L[1:], 1):
        t, b, m = M[f"t{k}"].astype(np.int64), M[f"b{k}"], M[f"m{k}"]
        last = k == len(L) - 1
        if l["kind"] == "conv":
            Hi, Ci, s = a.shape[1], a.shape[3], l["stride"]
            pp = np.zeros((N, Hi + 2, Hi + 2, Ci), np.int64)
            pp[:, 1:-1, 1:-1] = a
            Hc = (Hi + s - 1) // s
            acc = np.zeros((N, Hc, Hc, t.shape[0]), np.int64)
            for dy in range(3):
                for dx in range(3):
                    win = pp[:, dy:dy + s * Hc:s, dx:dx + s * Hc:s, :]
                    acc += np.einsum("nyxc,oc->nyxo", win, t[:, dy, dx, :])
            mx = np.abs(acc).max()
            assert mx < 32768, "int16 overflow"
            if l["pool"]:
                acc = acc.reshape(N, Hc // 2, 2, Hc // 2, 2, -1).max(axis=(2, 4))
            a = requant(acc, b, m, l["amax"])
        else:
            x = a.reshape(N, -1)
            acc = x @ t.T
            assert np.abs(acc).max() < 32768, "int16 overflow"
            a = requant(acc, b, m, None if last else l["amax"])
    return a


def load(path):
    d = np.load(path, allow_pickle=False)
    M = {k: d[k] for k in d.files}
    import json
    M["layers"] = json.loads(str(d["layers_json"]))
    return M


def export(pt_path, out_path):
    import json
    import torch
    from model import Net, ternary_t
    ck = torch.load(pt_path, map_location="cpu")
    spec = ck["spec"]
    net = Net(spec)
    net.load_state_dict(ck["state"])
    net.eval()
    sc = net.ascale.numpy().astype(np.float64)
    out = {}
    layers = []
    # layer 0: the lookup table, from the float conv + BN + ReLU + quantisation
    conv, bn = net.layers[0], net.bns[0]
    pats = np.array([[(i >> (8 - k)) & 1 for k in range(9)] for i in range(512)], np.float32).reshape(512, 1, 3, 3)
    with torch.no_grad():
        z = torch.nn.functional.conv2d(torch.from_numpy(pats), conv.weight, None, 1, 0)     # (512, C, 1, 1)
        z = bn(z).reshape(512, -1).numpy().astype(np.float64)
    out["lut"] = np.clip(np.floor(z / sc[0] + 0.5), 0, 127).astype(np.uint8)
    layers.append(dict(kind="lut", C=spec[0][1]))
    s_in = sc[0]
    shp_hw = 12
    for k, l in enumerate(spec[1:], 1):
        conv, bn = net.layers[k], net.bns[k]
        last = k == len(spec) - 1
        W = conv.weight.detach().double()
        t = ternary_t(W).numpy()
        dims = tuple(range(1, W.dim()))
        alpha = ((W.abs() * torch.from_numpy(np.abs(t))).sum(dim=dims) /
                 torch.from_numpy(np.abs(t)).sum(dim=dims).clamp(min=1)).numpy()
        if l[0] == "conv":
            g = bn.weight.detach().double().numpy()
            beta = bn.bias.detach().double().numpy()
            mu = bn.running_mean.double().numpy()
            sd = np.sqrt(bn.running_var.double().numpy() + bn.eps)
            # z = g * (alpha*s_in*acc - mu)/sd + beta ; q = z / s_out
            Mc = g * alpha * s_in / (sd * sc[k])
            bc = (beta * sd / g - mu) / (alpha * s_in)
            t = np.transpose(t, (0, 2, 3, 1))                      # (co, dy, dx, ci)
            amax = l[4]
            st = l[2]
            hw_c = (shp_hw + st - 1) // st
            shp_hw = hw_c // 2 if l[3] else hw_c
            layers.append(dict(kind="conv", C=l[1], stride=st, pool=bool(l[3]), amax=amax))
        else:
            bias = conv.bias.detach().double().numpy()
            if last:
                Mc = SCORE_UNIT * alpha * s_in
                amax = None
            else:
                Mc = alpha * s_in / sc[k]
                amax = 63
            bc = bias / (alpha * s_in)
            # torch flattens (C, H, W); the Mac's buffers are (H, W, C)
            Cprev = layers[-1]["C"]
            if t.shape[1] == Cprev * shp_hw * shp_hw:
                t = t.reshape(t.shape[0], Cprev, shp_hw, shp_hw).transpose(0, 2, 3, 1).reshape(t.shape[0], -1)
            shp_hw = 1
            layers.append(dict(kind="fc", C=l[1], amax=amax))
        neg = Mc < 0
        Mc = np.where(neg, -Mc, Mc)
        bc = np.where(neg, -bc, bc)
        t = np.where(neg.reshape((-1,) + (1,) * (t.ndim - 1)), -t, t)
        m = np.floor(Mc * 65536 + 0.5).astype(np.int64)
        assert (m < 32768).all() and (m > 0).all(), (k, m.min(), m.max())
        b = np.clip(np.floor(bc + 0.5), -20000, 20000).astype(np.int64)
        out[f"t{k}"] = t.astype(np.int8)
        out[f"b{k}"] = b
        out[f"m{k}"] = m
        s_in = sc[k]
    out["layers_json"] = np.array(json.dumps(layers))
    np.savez(out_path, **out)
    dens = [float(np.abs(out[f"t{k}"]).mean()) for k in range(1, len(spec))]
    print("exported", out_path, "layers", layers, "ternary density", [round(d, 3) for d in dens])


def eval_model(path, sets=("pd_clean", "pd_mouse", "uji_clean", "uji_mouse", "syn_mouse")):
    from strokes import CALC, CLASSES
    M = load(path)
    res = {}
    for name in sets:
        d = np.load(os.path.join(CALC, "build", "data", f"test_{name}.npz"))
        X = np.unpackbits(d["X"], axis=1).reshape(-1, G, G)
        Y = d["Y"].astype(np.int64)
        S = np.concatenate([forward_batch(M, X[i:i + 2000]) for i in range(0, len(X), 2000)])
        P = S.argmax(1)
        res[name] = float((P == Y).mean())
        top3 = float((np.argsort(-S, 1)[:, :3] == Y[:, None]).any(1).mean())
        conf = {}
        for y, p in zip(Y, P):
            if y != p:
                conf[(CLASSES[y], CLASSES[p])] = conf.get((CLASSES[y], CLASSES[p]), 0) + 1
        worst = sorted(conf.items(), key=lambda kv: -kv[1])[:6]
        print(f"{name}: top1 {res[name] * 100:.2f}% top3 {top3 * 100:.2f}% n={len(Y)} worst "
              + ", ".join(f"{a}->{b}:{n}" for (a, b), n in worst))
    return res


if __name__ == "__main__":
    if sys.argv[1] == "export":
        export(sys.argv[2], sys.argv[3])
    elif sys.argv[1] == "eval":
        eval_model(sys.argv[2])

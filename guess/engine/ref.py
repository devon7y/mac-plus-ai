#!/usr/bin/env python3
"""Guess My Doodle integer reference: exactly what the Mac computes (app/engine.c).

Input: the 32x32 binary drawing (raster.py). Every weighted layer is ternary and ends in the same
requantisation (RQ), done on the 68000 with muls.w / add.l / shifts:

    acc = wrap16(sum w * x)                    16-bit accumulator (add.w / sub.w), wraps
    p   = wrap32(acc * M_c + B_c)              muls.w #M (16x16 -> 32), add.l #B
    v   = wrap16(p >> S_c)                     arithmetic shift (swap + asr.w, or asr.l)
    out = clamp(v, 0, A)                       ReLU and the layer's ceiling; A = -1: out = v (logits)

T (first layer): out[y][x] = table[idx] with idx the 3x3 pattern around (y, x) (zeros outside),
bit 8 = top-left ... bit 0 = bottom-right. P: 2x2 max pool. G: global max. L: fully connected on
the source flattened channels-last (y, x, c).
"""
import numpy as np

import model_file


def wrap16(a):
    return ((np.asarray(a, np.int64) + 32768) & 0xFFFF) - 32768


def wrap32(a):
    return ((np.asarray(a, np.int64) + (1 << 31)) & 0xFFFFFFFF) - (1 << 31)


def rq(acc, l):
    acc = wrap16(acc)
    p = wrap32(acc * l["M"] + l["B"])
    v = wrap16(p >> l["S"])
    if l["A"] < 0:
        return v
    return np.clip(v, 0, l["A"])


def patterns(bits):
    """(G, G) 0/1 -> (G, G) 9-bit pattern indices."""
    G = bits.shape[0]
    b = np.pad(np.asarray(bits, np.int64), 1)
    idx = np.zeros((G, G), np.int64)
    for ky in range(3):
        for kx in range(3):
            idx |= b[ky:ky + G, kx:kx + G] << (8 - (ky * 3 + kx))
    return idx


def layer(l, x):
    k = l["kind"]
    if k == "T":
        st = l.get("stride", 1) or 1
        return l["table"][patterns(x)[::st, ::st]]
    if k == "P":
        H, W, C = x.shape
        return x.reshape(H // 2, 2, W // 2, 2, C).max(axis=(1, 3))
    if k == "G":
        return x.max(axis=(0, 1), keepdims=True)
    if k == "L":
        w = l["w"].reshape(l["cout"], l["cin"]).astype(np.int64)
        return rq(w @ x.reshape(-1), l).reshape(1, 1, -1)
    H, W, C = x.shape
    xp = np.pad(x, ((1, 1), (1, 1), (0, 0)))
    if k == "D":
        w = l["w"].reshape(C, 3, 3).astype(np.int64)
        acc = np.zeros((H, W, C), np.int64)
        for ky in range(3):
            for kx in range(3):
                acc += xp[ky:ky + H, kx:kx + W] * w[:, ky, kx]
        return rq(acc, l)
    if k == "C":
        kk, s, co = l["k"], l["stride"], l["cout"]
        w = l["w"].reshape(co, kk, kk, C).astype(np.int64)
        Ho, Wo = H // s, W // s
        if kk == 1:
            acc = x[::s, ::s].reshape(-1, C) @ w.reshape(co, C).T
            return rq(acc.reshape(Ho, Wo, co), l)
        acc = np.zeros((Ho, Wo, co), np.int64)
        for ky in range(kk):
            for kx in range(kk):
                patch = xp[ky:ky + H:s, kx:kx + W:s][:Ho, :Wo]
                acc += patch @ w[:, ky, kx, :].T
        return rq(acc, l)
    raise ValueError(k)


def run(m, bits, trace=None):
    """bits: (G, G) 0/1. Returns int64 logits (ncls,). trace: list to receive every tensor."""
    t = {0: np.asarray(bits, np.int64)}
    for l in m["layers"]:
        t[l["dst"]] = layer(l, t[l["src"]])
        if trace is not None:
            trace.append(t[l["dst"]])
    return t[m["layers"][-1]["dst"]].reshape(-1)


def checksum(x):
    """The Mac's tensor checksum: s = s * 31 + (uint16) v over the values in (y, x, c) order."""
    s = 0
    for v in np.asarray(x, np.int64).reshape(-1):
        s = (s * 31 + (int(v) & 0xFFFF)) & 0xFFFFFFFF
    return s


if __name__ == "__main__":
    import sys
    m = model_file.read(sys.argv[1])
    rng = np.random.RandomState(0)
    bits = (rng.rand(m["G"], m["G"]) < 0.1).astype(np.int64)
    print(run(m, bits)[:10])

#!/usr/bin/env python3
"""Dream integer reference: the exact arithmetic of one network pass on the Mac.

Every layer is ternary (weights -1/0/+1) and ends in the same requantisation (RQ):

    acc = wrap16(sum_i w_i * x_i)                       16-bit sum with wraparound
    v   = wrap16(asr16(acc, k_c) + b_c)                 per-channel shift k_c (asl if k_c < 0), bias b_c
    addh mode: v = wrap16(v + old)                      (old = the value being overwritten)
    out = TABLE[v]                                      the full int16 v indexes a 65536-entry clamp table
                                                        (68000: ext.l v; add.l v,v; move.w 0(a3,v.l)):
                                                        relu<m>: clamp(v, 0, m); lin: clamp(v, -127, 127);
                                                        res: clamp(e, -3640, 3640); raw: out = v (no table)

Layer kinds (arch.py): pw = 1x1 conv (gather 'plain' or 's2d' 2x2 space-to-depth; scatter
'plain' or 'd2s' 2x2 depth-to-space), dw = depthwise 3x3 (or centre tap only). Tensors are
int16 arrays (H+2, W+2, C) with a zero border.

random_model(cfg) makes the benchmark's random weights with the same xorshift32 stream as
the Mac (bench/main.c), so a Mac run can be checked without copying weights around.
"""
import numpy as np

import arch

TAB_RANGE = np.arange(-32768, 32768, dtype=np.int64)


def table(name):
    if name == "raw":
        return None
    if name.startswith("relu"):
        return np.clip(TAB_RANGE, 0, int(name[4:])).astype(np.int16)
    if name == "lin":
        return np.clip(TAB_RANGE, -arch.LIN_MAX, arch.LIN_MAX).astype(np.int16)
    if name.startswith("lin"):
        return np.clip(TAB_RANGE, -int(name[3:]), int(name[3:])).astype(np.int16)
    if name == "res":
        return np.clip(TAB_RANGE, -arch.RES_MAX, arch.RES_MAX).astype(np.int16)
    raise ValueError(name)


def wrap16(a):
    a = np.asarray(a, dtype=np.int64)
    return ((a + 32768) & 0xFFFF) - 32768


def shift16(acc, k):
    """asr.w #k (k > 0) or asl.w #-k (k < 0) on int16 values, per channel k (broadcast)."""
    acc = np.asarray(acc, dtype=np.int64)
    k = np.asarray(k, dtype=np.int64)
    right = acc >> np.clip(k, 0, 15)
    left = wrap16(acc << np.clip(-k, 0, 15))
    return np.where(k >= 0, right, left)


def requant(acc, k, b, tab, old=None):
    """k: (cout,) shifts, or (cout, 2) two-term codes (k1, e2): v = (acc >> k1) + s (acc >> k2),
    s = sign(e2), k2 = |e2| - 4 (e2 = 0: no second term)."""
    acc = wrap16(acc)
    k = np.asarray(k)
    if k.ndim == 2:
        k1, e2 = k[:, 0], k[:, 1]
        t2 = np.where(e2 != 0, shift16(acc, np.abs(e2) - 4), 0)
        t = wrap16(shift16(acc, k1) + np.sign(e2) * t2)
    else:
        t = shift16(acc, k)
    v = wrap16(t + b)
    if old is not None:
        v = wrap16(v + old)
    if tab is None:
        return v.astype(np.int16)
    return tab[v + 32768]


# ----------------------------------------------------------------------------- RNG
class XorShift32:
    def __init__(self, seed):
        self.x = (seed & 0xFFFFFFFF) or 1

    def next(self):
        x = self.x
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.x = x
        return x


def layer_seed(i):
    return (0x2545F491 ^ ((i + 1) * 0x9E3779B9)) & 0xFFFFFFFF


ZERO_THRESH = 21845        # P(w = 0) = 1/3 for the benchmark's random weights
# 4-bit bench weights: P(|w| = 0..7) = .20 .30 .22 .13 .08 .04 .02 .01 (cumulative, of 65536)
Q4_CUM = [13107, 32768, 47186, 55706, 60948, 63570, 64881, 65536]


def random_layer(i, l, zth=ZERO_THRESH):
    """Weights (cout, n) int8, shifts (cout,), biases (cout,), drawn exactly like bench/main.c."""
    r = XorShift32(layer_seed(i))
    cout = l["cout"]
    n = l["cin"] if l["kind"] == "pw" else (1 if l["center"] else 9)
    w = np.zeros((cout, n), dtype=np.int8)
    if l["kind"] == "dw" and l["center"]:
        w[:, 0] = 1
    elif l["kind"] == "pw" and l.get("wq") == "q4":
        for o in range(cout):
            for j in range(n):
                x = r.next()
                u = x >> 16
                a = next(m for m, c in enumerate(Q4_CUM) if u < c)
                w[o, j] = a if (x >> 15) & 1 else -a
    else:
        for o in range(cout):
            for j in range(n):
                x = r.next()
                if (x >> 16) >= zth:
                    w[o, j] = 1 if (x >> 15) & 1 else -1
    k = np.array([2 + ((r.next() >> 16) % 5) for _ in range(cout)], dtype=np.int64)
    b = np.array([((r.next() >> 16) & 127) - 64 for _ in range(cout)], dtype=np.int64)
    if l.get("gain2"):          # second terms: none 1/3, else +-(k2 + 4) with k2 = k1 + 1..4
        e2 = []
        for o in range(cout):
            x = r.next() >> 16
            if x % 3 == 0:
                e2.append(0)
            else:
                k2 = int(k[o]) + 1 + (x >> 2) % 4
                e2.append((k2 + 4) * (1 if x & 2 else -1))
        k = np.stack([k, np.array(e2, dtype=np.int64)], 1)
    return w, k, b


def random_input(T):
    H, W, C = T["xt"]
    r = XorShift32(0x01234567)
    x = np.zeros((H + 2, W + 2, C), dtype=np.int16)
    for y in range(H):
        for xx in range(W):
            for c in range(C):
                x[y + 1, xx + 1, c] = ((r.next() >> 16) % 4095) - 2047
    return x


# ----------------------------------------------------------------------------- layers
def run_pw(l, w, k, b, src, dst, tab):
    g = l["grid"]
    if l["gather"] == "s2d":
        inner = src[1:-1, 1:-1, :]
        Hs, Ws, Cs = inner.shape
        v = inner.reshape(Hs // 2, 2, Ws // 2, 2, Cs).transpose(0, 2, 1, 3, 4).reshape(Hs // 2, Ws // 2, 4 * Cs)
    else:
        v = src[1:-1, 1:-1, :]
    acc = wrap16(np.einsum("yxi,oi->yxo", v.astype(np.int64), w.astype(np.int64)))
    if l["scatter"] == "d2s":
        Cf = l["cout"] // 4
        tgt = dst[1:-1, 1:-1, :]
        Hf, Wf, _ = tgt.shape
        old = tgt.reshape(g, 2, g, 2, Cf).transpose(0, 2, 1, 3, 4).reshape(g, g, 4 * Cf).astype(np.int64)
        o = requant(acc, k, b, tab, old if l["mode"] == "addh" else None)
        dst[1:-1, 1:-1, :] = o.reshape(g, g, 2, 2, Cf).transpose(0, 2, 1, 3, 4).reshape(Hf, Wf, Cf)
    else:
        old = dst[1:-1, 1:-1, :].astype(np.int64)
        dst[1:-1, 1:-1, :] = requant(acc, k, b, tab, old if l["mode"] == "addh" else None)


def run_dw(l, w, k, b, src, dst, tab):
    g = l["grid"]
    s = src.astype(np.int64)
    if l["center"]:
        acc = s[1:-1, 1:-1, :] * w[:, 0].astype(np.int64)
    else:
        acc = np.zeros((g, g, l["cout"]), dtype=np.int64)
        for t in range(9):
            dy, dx = t // 3 - 1, t % 3 - 1
            acc += s[1 + dy:1 + dy + g, 1 + dx:1 + dx + g, :] * w[:, t].astype(np.int64)
    dst[1:-1, 1:-1, :] = requant(acc, k, b, tab)


def checksum(a):
    """s = s*31 + (v & 0xFFFF) mod 2^32 over the padded tensor in memory order (as the Mac)."""
    s = 0
    for v in a.reshape(-1).astype(np.int64) & 0xFFFF:
        s = (s * 31 + int(v)) & 0xFFFFFFFF
    return s


def forward(T, L, params, xt):
    """Run the layer program. Returns the tensor dict and the per-layer checksums of dst."""
    tens = {n: np.zeros((s[0] + 2, s[1] + 2, s[2]), dtype=np.int16) for n, s in T.items()}
    tens["xt"][:] = xt
    tabs = {}
    sums = []
    for i, l in enumerate(L):
        w, k, b = params[i]
        if l["tab"] not in tabs:
            tabs[l["tab"]] = table(l["tab"])
        (run_pw if l["kind"] == "pw" else run_dw)(l, w, k, b, tens[l["src"]], tens[l["dst"]], tabs[l["tab"]])
        sums.append(checksum(tens[l["dst"]]))
    return tens, sums


if __name__ == "__main__":
    import sys
    import time
    name = sys.argv[1] if len(sys.argv) > 1 else "A64S"
    T, L = arch.build(name)
    t0 = time.time()
    params = [random_layer(i, l) for i, l in enumerate(L)]
    xt = random_input(T)
    tens, sums = forward(T, L, params, xt)
    print(f"{name}: {len(L)} layers in {time.time() - t0:.1f} s")
    for l, s in zip(L, sums):
        d = tens[l["dst"]]
        print(f"  {l['tag']:10s} {s:08x}  range [{d.min()}, {d.max()}]  zeros {np.mean(d[1:-1, 1:-1] == 0):.2f}")

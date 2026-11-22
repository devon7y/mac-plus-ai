"""Quantise a trained checkpoint into Tune's integer model (the numbers the 68000 uses).

  python3 -I tune/ref/export.py tune/build/ckpt/final.pt tune/build/model   -> model.npz, model.bin

Integer format (SPEC.md):
  tables      int16 Q9 (1/512), entries clamped to +-TCLAMP so the 11 rows summed per step never
              overflow 16 bits
  W_hh, W_ih, W_out  ternary, per-row absmean scale alpha, packed in groups of 5 (W5 codes)
  M, k        per row M (<= 32767) and per matrix k:  ((acc * M) >> 16) << k  ~=  acc * alpha / 127
              in Q9 (Q8 for the output logits)
  bhn, b_ih   int16 Q9;  B_out int16 Q8
"""
import os
import struct
import sys

import numpy as np
import torch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "corpus"))
sys.path.insert(0, HERE)
import vocab as V  # noqa: E402
from tuneref import FEAT_NAMES, COND_NAMES, TCLAMP  # noqa: E402

K = 5                     # weights per group (W5)


def ternarize(w):
    a = np.maximum(np.abs(w).mean(axis=1), 1e-5)
    t = np.clip(np.round(w / a[:, None]), -1, 1).astype(np.int8)
    return t, a


def mult(real):
    """real multipliers -> (M int16 array, k): ((acc*M) >> 16) << k ~= acc * real."""
    for k in range(0, 8):
        M = np.round(real * 2.0 ** (16 - k))
        if M.max() <= 32767:
            return M.astype(np.int16), k
    raise ValueError("multiplier too large")


def w5_codes(T):
    """Ternary [rows][n] -> int16 codes (g*243 + idx)*2 - 32768, idx = sum (w_j+1)*3^j."""
    rows, n = T.shape
    G = -(-n // K)
    P = np.zeros((rows, G * K), np.int64)
    P[:, :n] = T
    pw = 3 ** np.arange(K)
    idx = ((P.reshape(rows, G, K) + 1) * pw).sum(axis=2)
    g = np.arange(G)
    return ((g * 243 + idx) * 2 - 32768).astype(np.int16), G


def q9(v):
    return np.clip(np.round(v * 512), -32767, 32767).astype(np.int16)


def export(ckpt):
    sd = {k: v.numpy() for k, v in torch.load(ckpt, map_location="cpu")["model"].items()}
    L = 2 if "l2.whh" in sd else 1
    p = {"L": L}
    for k in FEAT_NAMES + COND_NAMES:
        p["T_" + k] = np.clip(np.round(sd["emb.%s.weight" % k] * 512), -TCLAMP, TCLAMP).astype(np.int16)
    for l in range(L):
        pre = "l%d." % (l + 1)
        p["H%d" % l] = sd[pre + "whh"].shape[1]
        T, a = ternarize(sd[pre + "whh"])
        p["W_hh%d" % l] = T
        p["M_hh%d" % l], p["k_hh%d" % l] = mult(a * 512 / 127)
        p["bhn%d" % l] = q9(sd[pre + "bhn"])
        if l > 0:
            T, a = ternarize(sd[pre + "wih"])
            p["W_ih%d" % l] = T
            p["M_ih%d" % l], p["k_ih%d" % l] = mult(a * 512 / 127)
            p["b_ih%d" % l] = q9(sd[pre + "bih"])
    T, a = ternarize(sd["wout"])
    p["W_out"] = T
    p["M_out"], p["k_out"] = mult(a * 256 / 127)
    p["B_out"] = np.clip(np.round(sd["bout"] * 256), -32767, 32767).astype(np.int16)
    return p


def perm(H):
    """Interleave gate rows per unit: new row 3i+g = old row g*H+i (r, z, n)."""
    return np.array([g * H + i for i in range(H) for g in range(3)])


def write_bin(p, path):
    """Big-endian blob for the Mac (tune/app/engine.c tm_load). Gate rows and table columns are
    interleaved per unit (r z n), and each unit's multipliers are stored with its bhn (Mr Mz Mn bhn)."""
    L = p["L"]
    names = FEAT_NAMES + COND_NAMES
    H0 = p["H0"]
    P0 = perm(H0)
    hdr = struct.pack(">4sHHHH", b"TUNE", 2, L, V.VOCAB, len(names))
    for k in names:
        hdr += struct.pack(">H", p["T_" + k].shape[0])
    body = b""
    for k in names:
        body += p["T_" + k][:, P0].astype(">i2").tobytes()
    for l in range(L):
        H = p["H%d" % l]
        P = perm(H)
        codes, G = w5_codes(p["W_hh%d" % l][P])
        hdr += struct.pack(">HHH", H, G, p["k_hh%d" % l])
        mb = np.concatenate([p["M_hh%d" % l][P].reshape(H, 3), p["bhn%d" % l].reshape(H, 1)], axis=1)
        body += mb.astype(">i2").tobytes() + codes.astype(">i2").tobytes()
        if l > 0:
            ci, Gi = w5_codes(p["W_ih%d" % l][P])
            hdr += struct.pack(">HH", Gi, p["k_ih%d" % l])
            body += p["b_ih%d" % l][P].astype(">i2").tobytes() + p["M_ih%d" % l][P].astype(">i2").tobytes() \
                + ci.astype(">i2").tobytes()
    co, Go = w5_codes(p["W_out"])
    hdr += struct.pack(">HH", Go, p["k_out"])
    body += p["B_out"].astype(">i2").tobytes() + p["M_out"].astype(">i2").tobytes() + co.astype(">i2").tobytes()
    data = hdr + body
    open(path, "wb").write(data)
    return len(data), len(hdr)


def main():
    ckpt, out = sys.argv[1], sys.argv[2]
    p = export(ckpt)
    np.savez(out + ".npz", **p)
    n, h = write_bin(p, out + ".bin")
    print("layers", p["L"], "H", [p["H%d" % l] for l in range(p["L"])], "k_out", p["k_out"],
          "bin bytes", n, "header", h)


if __name__ == "__main__":
    main()

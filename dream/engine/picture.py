#!/usr/bin/env python3
"""Dream picture reference: one whole picture from a model file, exactly as the Mac makes it
(FORMAT.md: biases, noise, network passes, sampler, display levels). numpy only.

    picture.py <model.drm> <subject> <seed> [--png out.png] [--steps-png prefix]
"""
import math
import sys

import numpy as np

import model_file
import ref

RTAB = np.array([int(round(4096 * math.sqrt(-2 * math.log((i + 0.5) / 1024)))) for i in range(1024)], dtype=np.int64)
COS = np.array([int(round(16384 * math.cos(2 * math.pi * a / 1024))) for a in range(1024)], dtype=np.int64)
ORDER = np.array([[6, 1, 5], [3, 0, 8], [7, 4, 2]])


def xorshift(s):
    s ^= (s << 13) & 0xFFFFFFFF
    s ^= s >> 17
    s ^= (s << 5) & 0xFFFFFFFF
    return s


def noise(n, seed, return_state=False):
    """n Q9 values (n even), in storage order. With return_state, also the xorshift state after them
    (the re-noising sampler continues the same stream)."""
    s = (seed & 0xFFFFFFFF) or 1
    out = np.zeros(n, dtype=np.int64)
    for j in range(n // 2):
        s = xorshift(s)
        R = RTAB[s >> 22]
        a = (s >> 12) & 1023
        n1 = (R * COS[a]) >> 14
        n2 = (R * COS[(a + 768) & 1023]) >> 14
        out[2 * j] = min(2047, max(-2047, n1 >> 3))
        out[2 * j + 1] = min(2047, max(-2047, n2 >> 3))
    return (out, s) if return_state else out


def wrap16(a):
    return ((np.asarray(a, dtype=np.int64) + 32768) & 0xFFFF) - 32768


def subject_bias(M, s):
    acc = M["E"][s] @ M["U"]
    return ((acc >> 6) * M["Mc"]) >> 16


def step_bias(M, sb, i):
    return wrap16(M["B0"] + sb + M["BT"][i])


def net(M, xt_pad, B):
    params, o = [], 0
    for l, w, k in zip(M["L"], M["w"], M["k"]):
        params.append((w, k, B[o:o + l["cout"]]))
        o += l["cout"]
    tens, sums = ref.forward(M["T"], M["L"], params, xt_pad)
    return tens["out"], sums


def generate(M, subject, seed, trace=False):
    """Returns (list of x0 per step as (G, G, P*P) int arrays, final x0, details)."""
    H, W, C = M["T"]["xt"]
    x, state = noise(H * W * C, seed, return_state=True)
    x = x.reshape(H, W, C)
    sb = subject_bias(M, subject)
    x0s, det = [], []
    for i, sp in enumerate(M["samp"]):
        c1, c2 = sp[0], sp[1]
        c3 = sp[3] if len(sp) > 3 else 0
        B = step_bias(M, sb, i)
        xp = np.zeros((H + 2, W + 2, C), dtype=np.int16)
        sh = M.get("in_shift", 0)
        xp[1:-1, 1:-1] = np.clip(x >> sh, -(2047 >> sh), 2047 >> sh) if sh else x
        out, sums = net(M, xp, B)
        x0 = np.clip(out[1:-1, 1:-1].astype(np.int64), -512, 512)
        if c3:                       # fresh noise from the same stream (seeded by its state)
            z, state = noise(H * W * C, state, return_state=True)
            x = np.clip((c1 * x + c2 * x0 + c3 * z.reshape(H, W, C) + 8192) >> 14, -2047, 2047)
        else:
            x = np.clip((c1 * x + c2 * x0 + 8192) >> 14, -2047, 2047)
        x0s.append(x0)
        if trace:
            det.append(dict(B=B, sums=sums, x=x.copy()))
    return x0s, x0s[-1], det


def to_image(x0, patch):
    """(G, G, P*P) Q9 -> (G*P, G*P) Q9 picture."""
    G = x0.shape[0]
    return x0.reshape(G, G, patch, patch).transpose(0, 2, 1, 3).reshape(G * patch, G * patch)


def levels(img):
    """Display levels 0 (black) .. 9 (white): a contrast stretch of x0 in Q9 (clip art is mostly black
    and white; values within about 12% of either end show as solid): (i - 123) * 10 // 778, i = x0 + 512."""
    i = np.clip(np.asarray(img, dtype=np.int64) + 512, 0, 1024)
    return np.clip(((i - 123) * 10) // 778, 0, 9)


def halftone(img):
    g = levels(img)
    h, w = g.shape
    big = np.zeros((h * 3, w * 3), dtype=np.uint8)
    for v in range(3):
        for u in range(3):
            big[v::3, u::3] = (ORDER[v, u] < g) * 255
    return big


def checksum_bytes(b):
    s = 0
    for x in b:
        s = (s * 31 + x) & 0xFFFFFFFF
    return s


if __name__ == "__main__":
    path, subj, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    M = model_file.read(path)
    x0s, final, _ = generate(M, subj, seed)
    img = to_image(final, M["patch"])
    print(f"subject {subj} ({M['names'][subj]}), seed {seed}: final range [{img.min()}, {img.max()}], "
          f"levels {np.bincount(levels(img).reshape(-1), minlength=10).tolist()}")
    if "--png" in sys.argv:
        from PIL import Image
        Image.fromarray(halftone(img)).save(sys.argv[sys.argv.index("--png") + 1])

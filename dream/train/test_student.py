#!/usr/bin/env python3
"""Tests for student.py.
1. int mode == engine/ref.py, bit for bit, on the Mac bench's random ternary model (every tensor).
2. qat mode (float32 with straight-through rounding) == int mode on a calibrated float model
   converted to qat (no overflow), and the gradients flow.
Run: python3.11 dream/train/test_student.py [cfg]
"""
import os
import sys

import numpy as np
import torch
import torch.nn.functional as F

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "engine"))
import arch  # noqa: E402
import ref  # noqa: E402
from student import DreamStudent  # noqa: E402


def test_vs_ref(cfg):
    T, L = arch.build(cfg)
    m = DreamStudent(cfg, n_subjects=4)
    params = [ref.random_layer(i, l) for i, l in enumerate(L)]
    with torch.no_grad():
        for i, (w, k, b) in enumerate(params):
            m.W[i].copy_(torch.tensor(w, dtype=torch.float32))
            m.lam[i].copy_(torch.tensor(np.asarray(k)[..., 0] if np.ndim(k) == 2 else k, dtype=torch.float32))
            a, bb = m.offs[i]
            m.b0[a:bb] = torch.tensor(b, dtype=torch.float32)
        m.U.zero_()
    m.mode = "int"
    xt = ref.random_input(T)                                   # (H+2, W+2, 16) padded
    x = torch.tensor(xt[1:-1, 1:-1, :].astype(np.int64)).permute(2, 0, 1)[None].float()
    img = F.pixel_shuffle(x, arch.CONFIGS[cfg]["patch"])
    iparams = [(torch.tensor(np.asarray(w), dtype=torch.long).reshape(l["cout"], -1),
                torch.tensor(np.asarray(k), dtype=torch.long)) for (w, k, b), l in zip(params, L)]
    out = m.forward_int(img, torch.zeros(1, dtype=torch.long), torch.zeros(1), params=iparams)
    sh, mx = arch.in_shift(cfg)
    xt_ref = np.clip(xt.astype(np.int64) >> sh, -mx, mx).astype(np.int16) if sh else xt
    xt_ref[0, :, :] = 0; xt_ref[-1, :, :] = 0; xt_ref[:, 0, :] = 0; xt_ref[:, -1, :] = 0
    tens, _ = ref.forward(T, L, params, xt_ref)
    bad = 0
    for name, arr in tens.items():
        mine = m.last[name][0].permute(1, 2, 0).numpy()
        if not np.array_equal(mine, arr[1:-1, 1:-1, :]):
            bad += 1
            print(f"  tensor {name}: {np.sum(mine != arr[1:-1, 1:-1, :])} values differ")
    back = F.pixel_unshuffle(out, arch.CONFIGS[cfg]["patch"])[0].permute(1, 2, 0).numpy()
    bad += not np.array_equal(back, tens["out"][1:-1, 1:-1, :])
    print(f"{cfg} int mode vs ref.py: {'BIT-EXACT (all tensors)' if not bad else f'{bad} MISMATCHES'}")
    return bad


def test_qat_vs_int(cfg):
    torch.manual_seed(0)
    m = DreamStudent(cfg, n_subjects=8)
    B = 24          # >= 16 also exercises the CPU conv algorithm switch (NNPACK, disabled in student.py)
    img = torch.randn(B, 1, 64 if cfg.startswith("A64") else 32, 64 if cfg.startswith("A64") else 32) * 512
    s = torch.arange(B) % 8
    t = torch.rand(B)
    with torch.no_grad():
        m.U.normal_(0, 3)
        m.tmlp[2].weight.normal_(0, 0.5)
    m.calibrate(img, s, t)
    m.to_qat()
    out_q = m(img, s, t)
    tq = {k: v.detach().clone() for k, v in m.last.items()}
    m.mode = "int"
    out_i = m(img, s, t)
    bad = 0
    for k in tq:
        if not torch.equal(tq[k].long(), m.last[k].long()):
            bad += 1
            print(f"  tensor {k}: {(tq[k].long() != m.last[k].long()).sum().item()} values differ")
    rng = {k: (int(v.min()), int(v.max())) for k, v in m.last.items()}
    m.mode = "qat"
    loss = (m(img, s, t) / 512).pow(2).mean()
    loss.backward()
    gn = sum(p.grad.abs().sum().item() for p in m.parameters() if p.grad is not None)
    print(f"{cfg} qat vs int: {'IDENTICAL' if not bad else f'{bad} tensors differ'}; out range "
          f"{rng['out']}, h0 {rng['h0']}; grad L1 {gn:.3g}")
    return bad


if __name__ == "__main__":
    cfgs = sys.argv[1:] or ["A64S", "A64L2", "A64L2Q"]
    bad = 0
    for c in cfgs:
        bad += test_vs_ref(c)
        bad += test_qat_vs_int(c)
    print("ALL TESTS PASS" if not bad else "FAILURES")
    sys.exit(1 if bad else 0)

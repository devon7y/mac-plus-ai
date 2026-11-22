#!/usr/bin/env python3
"""Export training checkpoints (.pt) to the integer nets the Mac runs (.npz, model.save_int).

    export.py ckpt.pt out.npz [ckpt2.pt out2.npz ...]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from model import export_int, save_int  # noqa: E402
from ppo import load_actor  # noqa: E402

args = sys.argv[1:]
for src, dst in zip(args[::2], args[1::2]):
    a = load_actor(src)
    a.quant = True
    L = export_int(a)
    save_int(L, dst)
    nz = sum(int((l["T"] != 0).sum()) for l in L)
    print(f"{src} -> {dst}: {nz} nonzero ternary weights, shifts {[l['SH'] for l in L]}")

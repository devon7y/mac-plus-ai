#!/usr/bin/env python3
"""Check the float -> integer export (export.py, FORMAT.md) against the training model itself:
run paint/train/sketch_model.SketchDecoder (fake-quantized weights, float activations) and the
Mac's integer arithmetic (ref.py) side by side on the same drawing, teacher-forced, and report
how far the hidden state and the 123 outputs drift apart.

Usage: check_export.py [checkpoint.pt] [--H 256 --quant tern]   (no checkpoint: random init)
"""
import argparse
import os
import sys

import numpy as np
import torch

sys.dont_write_bytecode = True          # don't leave __pycache__ in paint/train (not our folder)
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "train"))
import export  # noqa: E402
import ref  # noqa: E402
from sketch_model import SketchDecoder  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("ckpt", nargs="?")
ap.add_argument("--H", type=int, default=256)
ap.add_argument("--quant", default="tern")
ap.add_argument("--scale", type=float, default=25.0)
ap.add_argument("--cat", type=int, default=64)
ap.add_argument("--steps", type=int, default=60)
ap.add_argument("--A", type=int, default=0, help="force A = AO (as export.py --A)")
a = ap.parse_args()
torch.manual_seed(0)
if a.ckpt:
    ck = torch.load(a.ckpt, map_location="cpu", weights_only=False)
    H, quant, S = int(ck["args"]["H"]), ck["args"]["quant"], float(ck["scale"])
    model = SketchDecoder(H, quant)
    model.load_state_dict(ck["model"])
else:
    H, quant, S = a.H, a.quant, a.scale
    model = SketchDecoder(H, quant)
    with torch.no_grad():                       # livelier than the default init
        for n in ("wh", "wx", "wc", "w0", "wo"):
            getattr(model, n).mul_(2.0)
        model.bo[60:100] -= 1.5
model.eval()
sd = {k: v.detach().double().numpy() for k, v in model.state_dict().items()}
m = export.state_model(sd, H, quant, S, export.names_from(export.NAMES), a.A or None)
print(f"H={H} quant={quant} A={m['A']} AO={m['AO']} SHR={m['SHR']} SHO={m['SHO']} SHA={m['SHA']} SHB={m['SHB']}")

rng = np.random.default_rng(1)
rows = [(int(rng.integers(-20, 21)), int(rng.integers(-20, 21)), int(rng.random() < 0.15)) for _ in range(a.steps)]
inputs = [(0, 0, 0)] + rows
# float model
with torch.no_grad():
    W, h, gc = model.step_consts(torch.tensor([a.cat]))
    fh, fo = [], []
    for dx, dy, pen in inputs:
        x5 = torch.tensor([[dx / S, dy / S, 1.0 - pen, float(pen), 0.0]], dtype=torch.float32)
        h, o = model.step(W, gc, h, x5)
        fh.append(h[0].double().numpy())
        fo.append(o[0].double().numpy())
# integer model
p = ref.model_params(m)
p["E"], h0 = ref.fold2(m, a.cat)
st = {"h": h0, "hq": (h0 * m["A"]) >> 14, "pen": 0, "dx": 0, "dy": 0}
ih, io = [], []
with torch.no_grad():
    fh0 = torch.tanh(W["emb"][a.cat] @ W["w0"].t() + model.b0).double().numpy()
print(f"h0: max |int/16384 - float| = {np.abs(h0 / 16384 - fh0).max():.4f}")
for dx, dy, pen in inputs:
    st["pen"], st["dx"], st["dy"] = pen, dx, dy
    ref.gru_step(p, st)
    _, O = ref.out_layer(p, (st["h"] * m["AO"]) >> 14)
    ih.append(st["h"] / 16384)
    io.append(O / 1024)
fh, fo, ih, io = map(np.array, (fh, fo, ih, io))
dh = np.abs(fh - ih)
print(f"hidden state over {len(inputs)} steps: mean |diff| {dh.mean():.4f}, max {dh.max():.4f} "
      f"(float |h| mean {np.abs(fh).mean():.3f})")
groups = {"pi logits": slice(0, 20), "mu x": slice(20, 40), "mu y": slice(40, 60), "log sigma x": slice(60, 80),
          "log sigma y": slice(80, 100), "rho raw": slice(100, 120), "pen logits": slice(120, 123)}
for g, sl in groups.items():
    d = np.abs(fo[:, sl] - io[:, sl])
    c = np.corrcoef(fo[:, sl].ravel(), io[:, sl].ravel())[0, 1]
    print(f"  {g:12s}: mean |diff| {d.mean():.4f}, max {d.max():.4f}, float spread {fo[:, sl].std():.3f}, corr {c:.4f}")
agree = np.mean(np.argmax(fo[:, 0:20], 1) == np.argmax(io[:, 0:20], 1))
agree_p = np.mean(np.argmax(fo[:, 120:123], 1) == np.argmax(io[:, 120:123], 1))
print(f"  most likely mixture component agrees {agree:.0%}, most likely pen state agrees {agree_p:.0%}")

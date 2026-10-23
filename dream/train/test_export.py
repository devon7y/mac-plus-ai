#!/usr/bin/env python3
"""Export round trip: a calibrated random student (qat) -> model file -> engine/picture.py must equal
export.mac_sample (the student in int mode with the Mac's sampler), bit for bit."""
import os
import sys

import torch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import export  # noqa: E402
from student import DreamStudent  # noqa: E402

cfg = sys.argv[1] if len(sys.argv) > 1 else "A64S"
torch.manual_seed(1)
m = DreamStudent(cfg, n_subjects=10)
with torch.no_grad():
    m.U.normal_(0, 3)
    m.tmlp[2].weight.normal_(0, 0.3)
    m.b0.normal_(0, 5)
img = torch.randn(8, 1, 64 if cfg.startswith("A64") else 32, 64 if cfg.startswith("A64") else 32) * 512
m.calibrate(img, torch.arange(8) % 10, torch.rand(8))
m.to_qat()
ts = [1 - i / 6 for i in range(7)]
out = os.path.join(HERE, "..", "build", "models", f"test_export_{cfg}.drm")
os.makedirs(os.path.dirname(out), exist_ok=True)
names = [f"thing {i}" for i in range(10)]
bad = 0
for c in (1.0, 0.0, 0.6):
    n = export.export(m, names, ts, out, drop=["thing 4"], c=c)
    print("wrote", out, n, f"bytes (dropped thing 4, sampler c = {c})")
    bad += export.check(m, out, ts, n=3, names=names, drop=["thing 4"], c=c)
print("EXPORT ROUND TRIP: " + ("BIT-EXACT" if not bad else "FAILED"))
sys.exit(1 if bad else 0)

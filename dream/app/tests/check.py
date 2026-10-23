#!/usr/bin/env python3
"""Check a Dream app run: the dumped final x0 must equal engine/picture.py's, bit for bit.
    check.py <model.drm> <run_dir> <subject> <seed> [--png out.png]"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "engine"))
import model_file  # noqa: E402
import picture  # noqa: E402

M = model_file.read(sys.argv[1])
run, subj, seed = sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
mac = np.frombuffer(open(os.path.join(run, "x0_mailbox.bin"), "rb").read(), dtype=">i2").astype(np.int64)
_, final, _ = picture.generate(M, subj, seed)
ok = np.array_equal(mac, final.reshape(-1))
print(f"subject {subj} seed {seed}: Mac picture {'BIT-EXACT' if ok else 'DIFFERS (%d values)' % np.sum(mac != final.reshape(-1))}")
s = 0
for v in final.reshape(-1):
    s = (s * 31 + (int(v) & 0xFFFF)) & 0xFFFFFFFF
print(f"reference picSum {s}")
if "--png" in sys.argv:
    from PIL import Image
    Image.fromarray(picture.halftone(picture.to_image(final, M["patch"]))).save(sys.argv[sys.argv.index("--png") + 1])
sys.exit(0 if ok else 1)

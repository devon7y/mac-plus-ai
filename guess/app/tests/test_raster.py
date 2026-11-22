#!/usr/bin/env python3
"""Host check: app/raster.c (built for the host with -DGHOST) == engine/raster.py on validation
drawings, whole and cut at random points. Usage: test_raster.py [N]"""
import ctypes
import os
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "engine"))
import raster  # noqa: E402

D = os.path.join(HERE, "..", "..", "data", "guess_mini")
lib = os.path.join(tempfile.mkdtemp(), "raster.so")
subprocess.run(["cc", "-O1", "-DGHOST", "-shared", "-fPIC", "-o", lib, os.path.join(HERE, "..", "raster.c")], check=True)
R = ctypes.CDLL(lib)
xy = np.load(D + "/valid_xy.npy")
pen = np.load(D + "/valid_pen.npy")
off = np.load(D + "/valid_off.npy")
rng = np.random.RandomState(3)
n = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
bad = 0
for k in range(n):
    i = rng.randint(len(off) - 1)
    rows = np.column_stack([xy[off[i]:off[i + 1]], pen[off[i]:off[i + 1]]])
    t = rng.randint(1, len(rows) + 2)
    xs, ys, up = raster.from_rows(rows, t)
    if max(xs) > 255 or max(ys) > 255:
        continue
    want = raster.pack(raster.rasterize(xs, ys, up))
    out = ctypes.create_string_buffer(128)
    m = len(xs)
    R.rasterize((ctypes.c_ubyte * m)(*xs), (ctypes.c_ubyte * m)(*ys), (ctypes.c_ubyte * m)(*up),
                ctypes.c_short(m), out)
    bad += out.raw != want
print(f"raster.c vs raster.py: {n} drawings, {bad} mismatches")

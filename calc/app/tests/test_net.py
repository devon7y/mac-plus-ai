#!/usr/bin/env python3
"""Bit-exactness of the generated C classifier (host build) against calc/ref/ref.py.

    python3 -I calc/app/tests/test_net.py <model.npz> <gen dir>   (gen dir from ref/gen_net.py)
Builds host_net.c + net_gen.c with clang -DNET_HOST and runs every held-out test raster through both.
"""
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CALC = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(CALC, "ref"))
import ref   # noqa: E402


def main():
    model, gen = sys.argv[1], sys.argv[2]
    exe = os.path.join(gen, "host_net")
    subprocess.run(["clang", "-O1", "-Wall", "-DNET_HOST", "-I", gen, "-o", exe,
                    os.path.join(HERE, "host_net.c"), os.path.join(gen, "net_gen.c")], check=True)
    M = ref.load(model)
    total, bad = 0, 0
    for name in ("pd_clean", "pd_mouse", "uji_clean", "uji_mouse", "syn_mouse"):
        d = np.load(os.path.join(CALC, "build", "data", f"test_{name}.npz"))
        X = d["X"]
        out = subprocess.run([exe], input=X.tobytes(), capture_output=True, check=True).stdout
        S_c = np.frombuffer(out, "<i2").reshape(len(X), -1).astype(np.int64)
        imgs = np.unpackbits(X, axis=1).reshape(-1, 24, 24)
        S_p = np.concatenate([ref.forward_batch(M, imgs[i:i + 2000]) for i in range(0, len(imgs), 2000)])
        nb = int((S_c != S_p).any(1).sum())
        total += len(X)
        bad += nb
        print(f"{name}: {len(X)} rasters, {nb} differ")
    print("RESULT", "PASS" if bad == 0 else "FAIL", f"({total} rasters)")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

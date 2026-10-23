#!/usr/bin/env python3
"""Build the host engine and check whole pictures bit-exactly against engine/picture.py.
    test_host.py <model.drm> [subject seed ...]"""
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "engine"))
import model_file  # noqa: E402
import picture  # noqa: E402

B = os.path.join(HERE, "..", "build", "host")
os.makedirs(B, exist_ok=True)
subprocess.run([sys.executable, os.path.join(HERE, "gen_tables.py"), B], check=True)
exe = os.path.join(B, "dream_host")
subprocess.run(["cc", "-O2", "-DDM_HOST", "-Wall", "-Wno-multichar", "-I", B, "-I", os.path.join(HERE, "..", "bench"),
                "-o", exe, os.path.join(HERE, "engine.c"), os.path.join(HERE, "host_main.c")], check=True)
path = sys.argv[1]
M = model_file.read(path)
pairs = sys.argv[2:] or ["0", "1", "3", "424242"]
bad = 0
for subj, seed in zip(pairs[::2], pairs[1::2]):
    out = os.path.join(B, "out.bin")
    r = subprocess.run([exe, path, subj, seed, out], capture_output=True, text=True)
    print(r.stdout.strip(), r.stderr.strip())
    d = np.frombuffer(open(out, "rb").read(), dtype=">i2").astype(np.int64)
    x0s, final, _ = picture.generate(M, int(subj), int(seed))
    n = x0s[0].size
    ok = True
    for i, x in enumerate(x0s):
        if not np.array_equal(d[i * n:(i + 1) * n], x.reshape(-1)):
            ok = False
            print(f"  step {i}: {np.sum(d[i * n:(i + 1) * n] != x.reshape(-1))} values differ")
    sb = picture.subject_bias(M, int(subj))
    if not np.array_equal(d[len(x0s) * n:], sb):
        ok = False
        print("  subject biases differ")
    bad += not ok
    print(f"subject {subj} seed {seed}: {'BIT-EXACT' if ok else 'MISMATCH'} ({len(x0s)} steps)")
print("ALL HOST PICTURES BIT-EXACT" if not bad else f"{bad} FAILURES")
sys.exit(1 if bad else 0)

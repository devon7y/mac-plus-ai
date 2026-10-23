#!/usr/bin/env python3
"""Host check of the Mac's layer compiler (bench/jit.c jit_pw_codes + jit_dw) against engine/jit.py
for every layer of a model file.   test_jit.py <model.drm>"""
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "engine"))
import jit  # noqa: E402
import model_file  # noqa: E402

B = os.path.join(HERE, "..", "build", "host")
os.makedirs(B, exist_ok=True)
exe = os.path.join(B, "host_jit")
subprocess.run(["cc", "-O2", "-Wall", "-Wno-multichar", "-I", os.path.join(HERE, "..", "bench"), "-o", exe,
                os.path.join(HERE, "host_jit.c"), os.path.join(HERE, "..", "bench", "jit.c")], check=True)
out = os.path.join(B, "jit_out.bin")
clamp = "--clamp" in sys.argv          # the 1 MB build's table-free depthwise code
subprocess.run([exe, sys.argv[1], out] + (["clamp"] if clamp else []), check=True)
M = model_file.read(sys.argv[1])
d = open(out, "rb").read()
pos, bad, total = 0, 0, 0
for i, l in enumerate(M["L"]):
    n = int.from_bytes(d[pos:pos + 4], "big")
    words = list(np.frombuffer(d[pos + 4:pos + 4 + 2 * n], dtype=">u2"))
    pos += 4 + 2 * n
    cl = model_file.table_range(l["tab"]) if (clamp and l["tab"] != "raw") else None
    if clamp and l["kind"] == "dw":
        ref = jit.emit_dw(l, M["T"], M["w"][i], M["k"][i], clamp=cl).w
    elif l["kind"] == "pw" and l.get("wq") == "q4":
        ref = jit.emit_pw(l, M["T"], M["w"][i], M["k"][i], clamp=cl).w
    elif clamp:                          # ternary 1x1 convs aren't compiled in the 1 MB build
        n = int.from_bytes(d[pos - 4 - 2 * n:pos - 2 * n], "big") if False else n
        ref = [int(x) for x in words]
    else:
        ref = jit.emit(dict(l, wq="tern"), M["T"], M["w"][i], M["k"][i]).w
    total += n
    if [int(x) for x in words] != ref:
        bad += 1
        print(f"layer {i} {l['kind']} {l['cin']}->{l['cout']}: C {n} words, python {len(ref)}")
print(f"{len(M['L'])} layers, {total * 2:,} bytes of code: " + ("ALL IDENTICAL TO jit.py" if not bad else f"{bad} DIFFER"))
sys.exit(1 if bad else 0)

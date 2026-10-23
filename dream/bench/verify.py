#!/usr/bin/env python3
"""Check a Mac bench run against the Python reference and report the timings.

    verify.py <cfg> <run_dir with log.txt and dump_*.bin> [--json out.json]

Checks (all must match exactly): the generated code of every layer (size and checksum, against
engine/jit.py), the input tensor, every layer's output checksum, and the output tensor.
"""
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "engine"))
import arch  # noqa: E402
import jit  # noqa: E402
import ref  # noqa: E402

TICK = 1 / 60.147


def parse(log):
    ent = []
    for line in open(log):
        if line.startswith("ENTRY "):
            p = line.split()
            ent.append((p[1], int(p[2]), int(p[3]), int(p[4])))
    return ent


def main():
    cfg, run = sys.argv[1], sys.argv[2]
    T, L = arch.build(cfg)
    ent = parse(os.path.join(run, "log.txt"))
    if not ent:
        sys.exit("no entries in log")
    params = [ref.random_layer(i, l) for i, l in enumerate(L)]
    xt = ref.random_input(T)
    tens, sums = ref.forward(T, L, params, xt)
    bad = 0
    # generated code
    codes = {e[1]: (e[2], e[3]) for e in ent if e[0] == "CODE"}
    for i, l in enumerate(L):
        w, k, b = params[i]
        em = jit.emit(l, T, w, k)
        cs = 0
        for x in em.w:
            cs = (cs * 31 + x) & 0xFFFFFFFF
        got = codes.get(i)
        if got != (len(em.w) * 2, cs):
            bad += 1
            print(f"CODE MISMATCH layer {i} {l['tag']}: mac {got}, python {(len(em.w) * 2, cs)}")
    # input / output tensors
    for idx, name in ((1, "xt"), (2, "out")):
        f = os.path.join(run, f"dump_{idx}.bin")
        mac = np.frombuffer(open(f, "rb").read(), dtype=">i2").reshape(tens[name].shape)
        if not np.array_equal(mac, tens[name]):
            bad += 1
            print(f"TENSOR MISMATCH {name}: {np.sum(mac != tens[name])} values differ")
    msums = {e[1]: e[2] for e in ent if e[0] == "SUM"}
    for i, l in enumerate(L):
        if msums.get(i) != sums[i]:
            bad += 1
            print(f"SUM MISMATCH layer {i} {l['tag']}: mac {msums.get(i)}, python {sums[i]}")
    print(f"{cfg}: {'ALL BIT-EXACT' if not bad else f'{bad} MISMATCHES'} "
          f"({len(L)} layers: code, checksums, input and output tensors)")

    # timings
    rep = {"cfg": cfg, "bitexact": not bad}
    for e in ent:
        if e[0] == "MEM":
            rep["freemem"], rep["maxmem"] = e[1], e[2]
        if e[0] == "JIT":
            rep["jit_s"], rep["code_bytes"], rep["freemem_after"] = e[1] * TICK, e[2], e[3]
        if e[0] == "RNG":
            rep["rng_s"] = e[1] * TICK
    steps = [e[1] * TICK for e in ent if e[0] == "STEP"]
    rep["step_s"] = steps
    layers = []
    tot_macs = 0
    for e in ent:
        if e[0] == "LAYR":
            l = L[e[1]]
            s = e[2] * TICK / e[3]
            m = arch.macs(l)
            tot_macs += m
            layers.append(dict(tag=l["tag"], kind=l["kind"], grid=l["grid"], cin=l["cin"], cout=l["cout"],
                               s=s, macs=m, est_s=arch.cycles(l) / 7833600))
    rep["layers"] = layers
    rep["sum_layers_s"] = sum(x["s"] for x in layers)
    rep["macs"] = tot_macs
    rep["params"] = sum(arch.params(l) for l in L)
    print(f"  pass: {', '.join(f'{s:.3f}' for s in steps)} s   sum of layers {rep['sum_layers_s']:.3f} s   "
          f"MACs {tot_macs / 1e6:.2f}M -> {tot_macs / rep['sum_layers_s'] / 1e6:.2f}M MAC/s   "
          f"params {rep['params'] / 1e3:.0f}K   code {rep.get('code_bytes', 0) / 1024:.0f} KB   "
          f"weights+JIT {rep.get('jit_s', 0):.1f} s (random weights alone {rep.get('rng_s', 0):.1f} s)")
    kinds = {}
    for x in layers:
        key = (x["kind"], x["grid"])
        a = kinds.setdefault(key, [0, 0, 0])
        a[0] += x["s"]; a[1] += x["macs"]; a[2] += x["est_s"]
    for (k, g), (s, m, es) in sorted(kinds.items()):
        print(f"  {k} grid {g:2d}: {s:6.3f} s  ({m / 1e6:5.2f}M MACs, {m / s / 1e6:5.2f}M MAC/s; model {es:6.3f} s)")
    if "--json" in sys.argv:
        json.dump(rep, open(sys.argv[sys.argv.index("--json") + 1], "w"), indent=1)
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())

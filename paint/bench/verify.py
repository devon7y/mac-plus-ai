#!/usr/bin/env python3
"""Check every configuration of the decoder benchmark bit-exactly against ref.py, using the
buffers the 68000 program handed to bench.lua: the category fold, one full decoder step
(all intermediates: recurrent sums, inputs, gates, new state, output layer, sample) and an
8-step sampled trace that feeds each sampled point back in.

Usage: verify.py <layout.json> <dump dir>
"""
import json
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ref  # noqa: E402

lay = json.load(open(sys.argv[1]))
ddir = sys.argv[2]


def words(b):
    return np.array(struct.unpack(f">{len(b) // 2}h", b), np.int64)


def longs(b):
    return np.array(struct.unpack(f">{len(b) // 4}i", b), np.int64)


def sbytes(b):
    return np.frombuffer(b, np.int8).astype(np.int64)


def scal(b):
    seed, = struct.unpack(">I", b[:4])
    pen, dx, dy, cat, invt, hlt, fq, A, shr, sho = struct.unpack(">10h", b[4:24])
    return dict(seed=seed, pen=pen, dx=dx, dy=dy, cat=cat, invt=invt, hlt=hlt, fq=fq, A=A, SHR=shr, SHO=sho)


def samp(b):
    w = struct.unpack(">12h", b[:24])
    x, y = struct.unpack(">2i", b[24:32])
    return {"k": w[0], "pen": w[1], "dx": w[2], "dy": w[3], "s1": w[6], "s2": w[7], "rho": w[8], "c": w[9],
            "n1": w[10], "n2": w[11], "x": x, "y": y}


def decode(kind, raw, rows, H):
    G = H // 4 if kind != "P4" else H // 2
    if kind == "W4":
        return ref.decode_w4(words(raw), rows, G)
    if kind == "B4":
        return ref.decode_b4(np.frombuffer(raw, np.uint8), rows, G)
    return ref.decode_p4(words(raw), rows, H)


ok_all = True
for cfg in lay["configs"]:
    if not cfg["run"]:
        continue
    name = f"{cfg['cell'].upper()} H={cfg['H']} {cfg['kind']}"
    ph = [{n: open(os.path.join(ddir, f"dump_{i}.bin"), "rb").read() for i, n, _ in p} for p in cfg["dumps"]]
    pre, post, tr = ph
    H, R = cfg["H"], cfg["R"]
    sc0 = scal(pre["SCAL"])
    p = {"cell": cfg["cell"], "H": H, "A": sc0["A"], "SHR": sc0["SHR"], "SHO": sc0["SHO"],
         "U": decode(cfg["kind"], pre["U"], R, H), "WY": decode(cfg["kind"], pre["WY"], lay["nout"], H),
         "M": words(pre["M"]), "MY": words(pre["MY"]), "BY": words(pre["BY"]), "E": words(pre["E"]),
         "P": words(pre["P"]).reshape(3, R), "WDX": words(pre["WDX"]), "WDY": words(pre["WDY"])}
    if cfg["cell"] == "gru":
        p["MN"] = words(pre["MN"]).reshape(H, 2)
    if cfg["kind"] == "P4":
        p["kind_wh"] = p["kind_wo"] = 2         # dual-accumulator, saturating kernel
    checks = []
    emb = sbytes(pre["EMB"]).reshape(lay["ncat"], lay["nemb"])[sc0["cat"]]
    E = ref.fold(sbytes(pre["WE"]).reshape(R, lay["nemb"]), words(pre["B0"]), emb)
    checks.append(("category fold", np.array_equal(E, p["E"])))
    h0 = words(pre["HST"])
    if cfg["kind"] == "P4":       # force_sat replaced hq by a constant to push one row past int16
        checks.append(("hq forced", len(set(words(pre["HQ"]).tolist())) == 1))
    else:
        checks.append(("hq init", np.array_equal((h0 * p["A"]) >> 14, words(pre["HQ"]))))
    st = {"h": h0, "hq": words(pre["HQ"]), "c": words(pre["C"]), "pen": sc0["pen"], "dx": sc0["dx"], "dy": sc0["dy"]}
    sc = {k: sc0[k] for k in ("seed", "invt", "hlt", "fq")}
    out = ref.full_step(p, st, sc)
    checks.append(("recurrent matvec", np.array_equal(out["S"], words(post["S"]))))
    if cfg["kind"] == "P4":
        sat = int((np.abs(np.asarray(p["U"], np.int64) @ np.asarray(words(pre["HQ"]), np.int64)) > 32767).sum())
        checks.append((f"saturating P4 sums exercised ({sat} rows clamped)", sat > 0 and int(words(post["S"])[0]) == 32767))
    checks.append(("input + fold", np.array_equal(out["CX"], longs(post["CX"]))))
    checks.append(("gates", np.array_equal(out["GV"], words(post["GV"]))))
    checks.append(("new h", np.array_equal(st["h"], words(post["HST"]))))
    checks.append(("new hq", np.array_equal(st["hq"], words(post["HQ"]))))
    if cfg["cell"] == "lstm":
        checks.append(("new c", np.array_equal(st["c"], words(post["C"]))))
    checks.append(("output matvec", np.array_equal(out["SY"], words(post["SY"]))))
    checks.append(("output layer", np.array_equal(out["O"], words(post["O"]))))
    ms = samp(post["SAMP"])
    checks.append(("sample", ms == out["samp"]))
    sc1 = scal(post["SCAL"])
    checks.append(("rng + next input", sc1["seed"] == sc["seed"] and (sc1["pen"], sc1["dx"], sc1["dy"]) ==
                   (st["pen"], st["dx"], st["dy"])))
    trace_ok, pts = True, []
    for t in range(lay["trace_n"]):
        o = ref.full_step(p, st, sc)
        m = samp(tr["TRACE"][32 * t:32 * t + 32])
        trace_ok &= m == o["samp"]
        pts.append((m["dx"], m["dy"], m["pen"]))
    trace_ok &= np.array_equal(st["h"], words(tr["HST"])) and scal(tr["SCAL"])["seed"] == sc["seed"]
    checks.append((f"{lay['trace_n']}-step trace", trace_ok))
    good = all(c for _, c in checks)
    ok_all &= good
    bad = [n for n, c in checks if not c]
    print(f"{name:16s} {'OK' if good else 'MISMATCH: ' + ', '.join(bad)}  ({len(checks)} checks; trace {pts[:4]}...)")
    if not good and os.getenv("VERBOSE"):
        print("  sample mac", ms, "\n  sample ref", out["samp"])

print("ALL KERNELS BIT-EXACT" if ok_all else "SOME KERNELS WRONG")
sys.exit(0 if ok_all else 1)

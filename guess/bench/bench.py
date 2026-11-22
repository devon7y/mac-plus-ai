#!/usr/bin/env python3
"""M0 engine benchmark: build a disk for one model, run it in MAME, check it bit-exact, report times.

  bench.py make  NAME (SPEC | --model FILE.gmdl) [--ncls 150]   -> guess/build/bench_NAME/{model,tests,disk}
  bench.py check NAME LOG                                         -> compare a bench.lua log with ref.py
Tests: 16 drawings from the validation data (whole and partial), rasterised by engine/raster.py.
"""
import argparse
import os
import struct
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(HERE, "..", "engine"))
import layout  # noqa: E402
import model_file  # noqa: E402
import raster  # noqa: E402
import ref  # noqa: E402

DATA = os.path.join(ROOT, "guess", "data", "guess_mini")


def test_bitmaps(n=16):
    xy = np.load(os.path.join(DATA, "valid_xy.npy"))
    pen = np.load(os.path.join(DATA, "valid_pen.npy"))
    off = np.load(os.path.join(DATA, "valid_off.npy"))
    rng = np.random.RandomState(7)
    pick = rng.choice(len(off) - 1, n, replace=False)
    imgs = []
    for j, i in enumerate(pick):
        rows = np.column_stack([xy[off[i]:off[i + 1]], pen[off[i]:off[i + 1]]])
        npts = len(rows) + 1
        t = npts if j % 2 == 0 else max(1, (npts * (j % 5 + 1)) // 6)
        xs, ys, up = raster.from_rows(rows, t)
        imgs.append(np.array(raster.rasterize(xs, ys, up), np.int64))
    return imgs


def make(a):
    out = os.path.join(ROOT, "guess", "build", "bench_" + a.name)
    os.makedirs(out, exist_ok=True)
    mpath = os.path.join(out, "model.gmdl")
    if a.model:
        open(mpath, "wb").write(open(a.model, "rb").read())
    else:
        model_file.write(mpath, layout.random_model(a.spec, a.ncls, 1))
    imgs = test_bitmaps()
    tb = struct.pack(">H", len(imgs)) + b"".join(raster.pack(im.tolist()) for im in imgs)
    tpath = os.path.join(out, "tests.bin")
    open(tpath, "wb").write(tb)
    model_file.write_macbinary(mpath, os.path.join(out, "GuessModel.bin"), "Guess Model")
    model_file.write_macbinary(tpath, os.path.join(out, "GuessTests.bin"), "Guess Tests", b"GTST")
    app = os.path.join(ROOT, "guess", "build", "bench", "GuessBench.bin")
    r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "toolchain", "template", "build_disk.py"),
                        "--app", app, "--out", os.path.join(out, "disk.img"), "--no-narrow", "--label", "Guess Bench",
                        "--extra", os.path.join(out, "GuessModel.bin"), os.path.join(out, "GuessTests.bin")],
                       capture_output=True, text=True)
    print(r.stdout[-400:], r.stderr[-400:])
    print(os.path.join(out, "disk.img"))


def check(a):
    out = os.path.join(ROOT, "guess", "build", "bench_" + a.name)
    m = model_file.read(os.path.join(out, "model.gmdl"))
    tb = open(os.path.join(out, "tests.bin"), "rb").read()
    n = struct.unpack(">H", tb[:2])[0]
    imgs = []
    for j in range(n):
        b = np.frombuffer(tb[2 + 128 * j:2 + 128 * (j + 1)], np.uint8)
        imgs.append(np.unpackbits(b).reshape(32, 32).astype(np.int64))
    ent = {}
    for line in open(a.log):
        if line.startswith("ENTRY "):
            _, tag, x, y, z = line.split()
            ent.setdefault(tag.strip(), []).append((int(x), int(y), int(z)))
    ok = True
    tr = []
    ref.run(m, imgs[0], tr)
    want = {i: ref.checksum(t) for i, t in enumerate(tr)}
    for i, s, _ in ent.get("TSUM", []):
        if want[i] != s:
            ok = False
            print(f"layer {i} ({m['layers'][i]['kind']}) tensor checksum MISMATCH: mac {s:08x} ref {want[i]:08x}")
    ticks = []
    for j, s, bt in ent.get("TEST", []):
        lo = ref.run(m, imgs[j])
        if ref.checksum(lo) != s or int(np.argmax(lo)) != bt >> 16:
            ok = False
            print(f"test {j} logits MISMATCH")
        ticks.append(bt & 0xFFFF)
    print(f"{len(ent.get('TSUM', []))} tensors and {len(ent.get('TEST', []))} test drawings: "
          f"{'BIT-EXACT' if ok else 'MISMATCH'}")
    jit = ent["JIT"][0]
    read = ent["READ"][0]
    print(f"model file {m['size']} B; read {read[0]} ticks; compile {jit[0]} ticks ({jit[0] / 60.15:.2f} s), "
          f"code {jit[1]} B, tensors {jit[2]} B; FreeMem after load {ent['FREE'][0][0]} B "
          f"(at start {ent['MEM'][0][0]} B)")
    p = ent["PASS"][0]
    print(f"whole pass: {p[0] / p[1]:.1f} ticks = {p[0] / p[1] / 60.15:.3f} s; per test (ticks): {ticks}")
    tot = 0
    for i, t, k in ent.get("LAYR", []):
        l = m["layers"][i]
        ms = t / k / 60.15 * 1000
        tot += ms
        print(f"  layer {i:2d} {l['kind']} {l['cin']:4d}->{l['cout']:4d}  {ms:7.2f} ms")
    print(f"  sum of layers {tot:.1f} ms")
    if "TTAB" in ent:
        t = ent["TTAB"][0]
        print(f"  first layer in one call: {t[0] / t[1] / 60.15 * 1000:.1f} ms")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd")
    ap.add_argument("name")
    ap.add_argument("spec", nargs="?")
    ap.add_argument("--model", default="")
    ap.add_argument("--ncls", type=int, default=150)
    ap.add_argument("--log", default="")
    a = ap.parse_args()
    make(a) if a.cmd == "make" else check(a)

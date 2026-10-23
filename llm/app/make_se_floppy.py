#!/usr/bin/env python3
"""1.44 MB floppies (HFS, not bootable) with Pixel for the Macintosh SE (System 7, SuperDrive).

  make_se_floppy.py --model m.pxm --app build/PixelSE.bin --out DIR [--layout pair|single]
                    [--raw] [--test "seed T k maxnew variant" "line" ...]

pair (default): DIR/pixel_se_144_1.img ("Pixel 1": PixelSE, Pixel Model 1, Read Me) and
DIR/pixel_se_144_2.img ("Pixel 2": Pixel Model 2). single: DIR/pixel_se_144.img with everything
(only if it fits). --raw stores the model uncompressed (loads faster; fine for the pair).
Prints each volume's free space (hvol) after copying. hfsutils runs with a private HOME.
"""
import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "ref"))
import export  # noqa: E402
import make_disks as MD  # noqa: E402

SIZE = 1474560
README = "Copy everything from both Pixel disks into one folder on the hard disk, then double-click PixelSE.\r"


def run(cmd, env):
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"failed: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return r.stdout


def build(img, label, files, env):
    open(img, "wb").write(b"\0" * SIZE)
    run(["hformat", "-l", label, img], env)
    run(["hmount", img], env)
    try:
        for f in files:
            run(["hcopy", "-m", f, ":"], env)
        vol = run(["hvol"], env)
        names = run(["hls", "-l"], env)
    finally:
        subprocess.run(["humount"], env=env, capture_output=True)
    free = int([l for l in vol.splitlines() if "free" in l][0].split()[2])
    print(f"{img}: volume '{label}', {free:,} bytes free\n{names.rstrip()}")
    return free


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--app", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--layout", choices=("pair", "single", "app"), default="pair")
    ap.add_argument("--raw", action="store_true", help="uncompressed model (faster load)")
    ap.add_argument("--test", nargs="*")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="sefloppy_")
    env = dict(os.environ, HOME=tempfile.mkdtemp(prefix="hfs_home_"))
    run([sys.executable, "-I", os.path.join(MD.ROOT, "toolchain", "icons", "add_icon.py"), a.app, "--preset", "pixel",
         "--out", os.path.join(work, os.path.basename(a.app))], env)
    a.app = os.path.join(work, os.path.basename(a.app))   # the app with its Finder icon (toolchain/icons)
    pm = export.read_model(a.model)
    model = export.to_bytes(pm, huff=not a.raw)
    half = (len(model) // 2) & ~511
    parts = []
    for i, (lo, hi) in enumerate(((0, half), (half, len(model)))):
        p = os.path.join(work, f"Pixel Model {i + 1}.bin")
        MD.macbin(p, f"Pixel Model {i + 1}", b"PXMD", MD.part(model, i + 1, lo, hi))
        parts.append(p)
    readme = os.path.join(work, "Read Me.bin")
    hdr = bytearray(128)
    nm = b"Read Me"
    hdr[1] = len(nm)
    hdr[2:2 + len(nm)] = nm
    hdr[65:69], hdr[69:73] = b"TEXT", b"ttxt"
    MD.rsrc.write_macbinary(readme, bytes(hdr), README.encode("mac_roman"), b"")
    extra = []
    if a.test:
        t = os.path.join(work, "Pixel Test.bin")
        MD.macbin(t, "Pixel Test", b"TEXT", "\r".join(a.test).encode("mac_roman") + b"\r")
        extra.append(t)
    print(f"model {len(model):,} bytes ({'raw' if a.raw else 'Huffman-coded'}), app {os.path.getsize(a.app):,}")
    if a.layout == "app":           # just the app, to replace PixelSE on the SE's hard disk
        build(os.path.join(a.out, "pixelse_app_144.img"), "PixelSE", [a.app], env)
    elif a.layout == "pair":
        build(os.path.join(a.out, "pixel_se_144_1.img"), "Pixel 1", [a.app, parts[0], readme] + extra, env)
        build(os.path.join(a.out, "pixel_se_144_2.img"), "Pixel 2", [parts[1]], env)
    else:
        build(os.path.join(a.out, "pixel_se_144.img"), "Pixel", [a.app] + parts + [readme] + extra, env)


if __name__ == "__main__":
    main()

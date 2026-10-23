#!/usr/bin/env python3
"""Make Pixel's floppy set (Plus) and SE folder from a model file (llm/ref/export.py format v2).

  make_disks.py --model m.pxm --app build/Pixel.bin --out DIR [--test "seed T k maxnew variant" "line" ...]
                [--no-narrow]

Boot disk (DIR/pixel_boot.img): trimmed System 6, Narrow Screen, Pixel, "Pixel Model 1" (as much of
the model as fits while leaving RESERVE bytes free) and, with --test, a "Pixel Test" script.
Disk 2 (DIR/pixel_disk2.img, volume "Pixel Disk 2"): "Pixel Model 2", the rest.
SE folder (DIR/se/): Pixel.bin plus both parts as MacBinary files (copy them into one folder).
Each part starts with a 16-byte header: 'PXPT', 0, part, u16 parts, u32 model id (body checksum),
u32 offset of the part in the model file.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "mactools"))
import rsrc  # noqa: E402

RESERVE = 15360          # bytes left free on the boot disk (System 6 writes there; manager: >= 15 KB)
SLACK2 = 4096            # bytes left free on disk 2 (manager's rule: at least 4 KB)
DISK2_FREE = 803840      # an empty 800K HFS floppy
CREATOR = b"Pxl1"


def macbin(path, name, ftype, data):
    hdr = bytearray(128)
    nm = name.encode("mac_roman")
    hdr[1] = len(nm)
    hdr[2:2 + len(nm)] = nm
    hdr[65:69], hdr[69:73] = ftype, CREATOR
    rsrc.write_macbinary(path, bytes(hdr), data, b"")


def run(cmd, env):
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"failed: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return r.stdout


def free_bytes(img, env):
    run(["hmount", img], env)
    try:
        vol = run(["hvol"], env)
    finally:
        subprocess.run(["humount"], env=env, capture_output=True)
    return int([l for l in vol.splitlines() if "free" in l][0].split()[2])


def part(model, idx, lo, hi):
    mid = model[40:44]       # body checksum = model id
    return b"PXPT" + bytes([0, idx]) + struct.pack(">H", 2) + mid + struct.pack(">I", lo) + model[lo:hi]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--app", required=True, help="the Plus build (floppies)")
    ap.add_argument("--se-app", help="the SE build (hard disk); default: --app")
    ap.add_argument("--out", required=True)
    ap.add_argument("--test", nargs="*")
    ap.add_argument("--part1", type=int, help="force the size of part 1 (bytes of model)")
    ap.add_argument("--slack2", type=int, default=SLACK2, help="bytes to leave free on disk 2")
    ap.add_argument("--tokenizer", help="tokenizer.json (llm/train/tokenizer.py) to embed if the model has none")
    ap.add_argument("--no-narrow", action="store_true", help="boot disk without Narrow Screen (test disks: 512 px)")
    a = ap.parse_args()
    model = open(a.model, "rb").read()
    assert model[:4] == b"PIXL"
    sys.path.insert(0, os.path.join(os.path.dirname(HERE), "ref"))
    import export
    pm = export.from_bytes(model)
    if a.tokenizer:
        pm["merges"] = export.tokenizer_merges(export.load_tokenizer(a.tokenizer))
    if len(pm.get("merges", [])) < 100:
        sys.exit("the model file has no tokenizer: pass --tokenizer llm/train/tokenizer.json (or the run's copy)")
    os.makedirs(a.out, exist_ok=True)
    work = os.path.join(a.out, "work")
    os.makedirs(os.path.join(work, "icon"), exist_ok=True)
    home = tempfile.mkdtemp(prefix="hfs_home_")
    env = dict(os.environ, HOME=home)
    for k in ("app", "se_app"):                     # the apps with their Finder icon (toolchain/icons)
        if getattr(a, k):
            dst = os.path.join(work, "icon", os.path.basename(getattr(a, k)))
            run([sys.executable, "-I", os.path.join(ROOT, "toolchain", "icons", "add_icon.py"), getattr(a, k),
                 "--preset", "pixel", "--out", dst], env)
            setattr(a, k, dst)
    try:
        extras = []
        if a.test:
            t = os.path.join(work, "Pixel Test.bin")
            macbin(t, "Pixel Test", b"TEXT", "\r".join(a.test).encode("mac_roman") + b"\r")
            extras.append(t)
        boot = os.path.join(a.out, "pixel_boot.img")
        # 1. boot disk without the model, to measure the space left
        narrow = ["--no-narrow"] if a.no_narrow else []
        run([sys.executable, "-I", os.path.join(HERE, "build_disk.py"), "--app", a.app, "--out", boot,
             "--label", "Pixel"] + narrow + (["--extra"] + extras if extras else []), env)
        free = free_bytes(boot, env)
        # Huffman-code only as many ternary matrices as needed (each costs decoding time)
        budget = ((free - RESERVE - 512) // 512) * 512 - 16 + ((DISK2_FREE - a.slack2) // 512) * 512 - 16 - 512
        model = export.to_bytes(pm, budget=budget)
        if len(model) > budget:
            sys.exit(f"model {len(model):,} bytes does not fit the floppy budget {budget:,}")
        n1 = a.part1 if a.part1 else ((free - RESERVE - 512) // 512) * 512 - 16
        print(f"boot disk free before the model: {free:,}; budget {budget:,}; model re-encoded to {len(model):,} "
              f"bytes; part 1 gets {n1:,}")
        n1 = min(n1, len(model))
        p1 = os.path.join(work, "Pixel Model 1.bin")
        macbin(p1, "Pixel Model 1", b"PXMD", part(model, 1, 0, n1))
        run([sys.executable, "-I", os.path.join(HERE, "build_disk.py"), "--app", a.app, "--out", boot,
             "--label", "Pixel"] + narrow + ["--extra", p1] + extras, env)
        left = free_bytes(boot, env)
        # 2. disk 2
        p2 = os.path.join(work, "Pixel Model 2.bin")
        macbin(p2, "Pixel Model 2", b"PXMD", part(model, 2, n1, len(model)))
        d2 = os.path.join(a.out, "pixel_disk2.img")
        open(d2, "wb").write(b"\0" * 819200)
        run(["hformat", "-l", "Pixel Disk 2", d2], env)
        print(f"disk 2: part 2 needs {len(model) - n1 + 16:,} bytes (+128-byte MacBinary wrapper), "
              f"empty disk has {free_bytes(d2, env):,}")
        run(["hmount", d2], env)
        try:
            run(["hcopy", "-m", p2, ":"], env)
            vol2 = run(["hvol"], env)
        finally:
            subprocess.run(["humount"], env=env, capture_output=True)
        free2 = int([l for l in vol2.splitlines() if "free" in l][0].split()[2])
        # 3. SE: no floppy limit, so an uncompressed model (B5 bytes, faster to load) in two parts
        se = os.path.join(a.out, "se")
        os.makedirs(se, exist_ok=True)
        raw = export.to_bytes(pm, huff=False)
        half = (len(raw) // 2) & ~511
        se_app = a.se_app or a.app
        se_files = [os.path.join(se, os.path.basename(se_app))]
        shutil.copy(se_app, se_files[0])
        for i, (lo, hi) in enumerate(((0, half), (half, len(raw)))):
            f = os.path.join(se, f"Pixel Model {i + 1}.bin")
            macbin(f, f"Pixel Model {i + 1}", b"PXMD", part(raw, i + 1, lo, hi))
            se_files.append(f)
        se_extra = se_files[1:] + extras
        hd = os.path.join(a.out, "pixel_se7.hd")
        r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "toolchain", "se7", "build_se_disk.py"),
                            "--app", se_app, "--out", hd, "--extra"] + se_extra, capture_output=True, text=True)
        print(f"SE hard disk: {hd}" if r.returncode == 0 else f"SE hard disk not built: {r.stderr.strip()[-200:]}")
        print(f"model {len(model):,} bytes: part 1 {n1:,} (boot disk, {left:,} bytes left free), "
              f"part 2 {len(model) - n1:,} (disk 2, {free2:,} bytes left free); app {os.path.getsize(a.app):,}")
        if left < RESERVE:
            sys.exit(f"boot disk has only {left} bytes free (< {RESERVE})")
    finally:
        shutil.rmtree(home, ignore_errors=True)


if __name__ == "__main__":
    main()

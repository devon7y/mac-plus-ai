#!/usr/bin/env python3
"""Build Rail Runner: generate data, compile with Retro68, make a boot floppy.

    python3 runner/build.py [--size 320x200] [--defs BENCH,REPLAY] [--tag NAME] [--no-disk]

Outputs in runner/build/<tag>/: gen/ (generated C data), cmake/ (build tree, RailRunner.bin),
and <tag>.img (800K boot disk: trimmed System 6.0 + Narrow Screen + the game, which starts
straight away). The default tag is "game" (320x200, no defines; 384x240 measured too slow for a real Plus).
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TC = os.path.expanduser("~/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake")


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f"FAILED: {' '.join(cmd)}\n{r.stdout[-4000:]}\n{r.stderr[-4000:]}")
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", default="320x200")
    ap.add_argument("--defs", default="")
    ap.add_argument("--tag", default=None)
    ap.add_argument("--no-disk", action="store_true")
    ap.add_argument("--extra", nargs="*", default=[])
    a = ap.parse_args()
    w, h = (int(v) for v in a.size.split("x"))
    tag = a.tag or "game"
    out = os.path.join(HERE, "build", tag)
    gen = os.path.join(out, "gen")
    os.makedirs(gen, exist_ok=True)
    print(run([sys.executable, os.path.join(HERE, "tools", "gen_data.py"), gen, "--w", str(w), "--h", str(h)]).strip())
    if "REPLAY" in a.defs:
        print(run([sys.executable, os.path.join(HERE, "proto", "game.py"), "replay", gen]).strip())
    cm = os.path.join(out, "cmake")
    defs = ";".join(d for d in a.defs.split(",") if d)
    src = os.environ.get("RR_SRC", os.path.join(HERE, "game"))   # (testing: another source tree)
    run(["cmake", "-S", src, "-B", cm, "-G", "Ninja",
         f"-DCMAKE_TOOLCHAIN_FILE={TC}", f"-DGEN_DIR={gen}", f"-DDEFS={defs}"])
    print(run(["cmake", "--build", cm]).strip().splitlines()[-1])
    app = os.path.join(cm, "RailRunner.bin")
    print(run([sys.executable, "-I", os.path.join(os.path.dirname(HERE), "toolchain", "icons", "add_icon.py"), app,
               "--preset", "railrunner"]).strip())                     # Finder icon + bundle
    size = run([os.path.expanduser("~/Retro68-build/toolchain/bin/m68k-apple-macos-size"),
                os.path.join(cm, "RailRunner.code.bin.gdb")])
    print(size.strip())
    print(f"app {os.path.getsize(app)} bytes -> {app}")
    if not a.no_disk:
        img = os.path.join(out, f"{tag}.img")
        extra = []
        for e in a.extra:
            extra += ["--extra", e]
        # --keep-se: the same floppy also boots a Macintosh SE (the lab's), at a cost of 15.7 KB of disk
        print(run([sys.executable, "-I", os.path.join(HERE, "tools", "build_disk.py"), "--app", app,
                   "--out", img, "--label", "Rail Runner", "--keep-se"] + extra).strip())
        print("disk ->", img)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Build Blockworld: generate tables, compile with Retro68, make boot floppies.

    build.py [--target BWBench] [--se] [--world FILE.bin | --no-world]

Outputs in blockworld/build/: gen/ (generated C), cmake/ (CMake build: <target>.bin),
<target>.img (800K Plus boot floppy: trimmed System + Narrow Screen + the app; for the game
also the starting world "Blockworld Default", MacBinary, by default build/release/Blockworld
Default.bin, made by make_release.py).
"""
import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(HERE, "build")
GEN = os.path.join(BUILD, "gen")
TC = os.path.expanduser("~/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake")


def run(*cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode:
        sys.exit(f"failed: {' '.join(cmd)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="BWBench")
    ap.add_argument("--se", action="store_true", help="keep the SE patch in the System (floppy boots an SE too)")
    ap.add_argument("--world", default=os.path.join(BUILD, "release", "Blockworld Default.bin"),
                    help="the starting world (MacBinary) put next to the game")
    ap.add_argument("--no-world", action="store_true", help="no starting world (the game makes seed 2's)")
    ap.add_argument("--out", help="the disk image (default build/<target>.img)")
    a = ap.parse_args()
    os.makedirs(GEN, exist_ok=True)
    run(sys.executable, os.path.join(HERE, "app", "gen_tables.py"), GEN)
    run(sys.executable, os.path.join(HERE, "app", "gen_items.py"), GEN)
    run(sys.executable, os.path.join(HERE, "app", "gen_mobs.py"), GEN)
    run(sys.executable, os.path.join(HERE, "tests", "gen_bench.py"), GEN, stdout=subprocess.DEVNULL)
    cm = os.path.join(BUILD, "cmake")
    if not os.path.exists(os.path.join(cm, "build.ninja")):
        run("cmake", "-S", os.path.join(HERE, "app"), "-B", cm, "-G", "Ninja",
            f"-DCMAKE_TOOLCHAIN_FILE={TC}", f"-DGEN_DIR={GEN}")
    run("cmake", "--build", cm)
    app = os.path.join(cm, a.target + ".bin")
    if a.target == "Blockworld":                    # Finder icon + bundle (toolchain/icons)
        run(sys.executable, "-I", os.path.join(ROOT, "toolchain", "icons", "add_icon.py"), app, "--preset", "blockworld")
    img = a.out or os.path.join(BUILD, a.target.lower() + ".img")
    cmd = [sys.executable, "-I", os.path.join(ROOT, "toolchain", "template", "build_disk.py"),
           "--app", app, "--out", img, "--label", "Blockworld"]
    if a.se:
        cmd.append("--keep-se")
    if a.target == "Blockworld" and not a.no_world:
        if not os.path.exists(a.world):
            sys.exit(f"no starting world {a.world} (make_release.py makes it; or --no-world)")
        cmd += ["--extra", a.world]
    run(*cmd)
    if a.target == "Blockworld" and not a.no_world:      # the starting world: locked (read only)
        env = dict(os.environ, HOME=tempfile.mkdtemp(prefix="hfs_home_"))
        run("hmount", img, env=env, stdout=subprocess.DEVNULL)
        try:
            run("hattrib", "+l", ":System Folder:Blockworld Default", env=env)
            free = subprocess.run(["hvol"], env=env, capture_output=True, text=True).stdout
        finally:
            subprocess.run(["humount"], env=env, capture_output=True)
        print([l.strip() for l in free.splitlines() if "free" in l][0])
    print("built", img, os.path.getsize(app), "bytes app")


if __name__ == "__main__":
    main()

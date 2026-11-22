#!/usr/bin/env python3
"""Build Orbit Duel: art tables, the app, its Finder icon and the disks.

    python3 -I duel/build.py [--no-disks]

Outputs (duel/build/, gitignored):
  app/OrbitDuel.bin           the Retro68 build (MacBinary)
  Orbit Duel.bin              the app named "Orbit Duel", with its Finder icon
  orbitduel.img               800K boot floppy for the Plus (trimmed System 6.0 + Narrow Screen; also
                              boots an SE, --keep-se), starts straight into the game
  orbitduel_se.img            1.44 MB boot floppy for the SE (System 6.0.7, tools/make_se_floppy.py)
  orbitduel_se7.hd            System 7.0.1 SE test hard disk (app in Startup Items)
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
B = os.path.join(HERE, "build")
TC = os.path.expanduser("~/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake")
sys.path.insert(0, os.path.join(ROOT, "mactools"))
import rsrc  # noqa: E402

APP_NAME = b"Orbit Duel"
ICON = os.path.join(ROOT, "toolchain", "icons", "art", "orbitduel.txt")


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f"failed: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-disks", action="store_true")
    ap.add_argument("--defs", default="", help="extra C defines for a test build, e.g. -DSELFTEST")
    a = ap.parse_args()
    os.makedirs(B, exist_ok=True)
    gen = os.path.join(B, "gen")
    print(run([sys.executable, "-I", os.path.join(HERE, "tools", "gen_art.py"), gen]).strip())
    bdir = os.path.join(B, "app")
    if not os.path.exists(os.path.join(bdir, "build.ninja")):
        run(["cmake", "-S", os.path.join(HERE, "app"), "-B", bdir, "-G", "Ninja", f"-DCMAKE_TOOLCHAIN_FILE={TC}",
             f"-DGEN_DIR={gen}"])
    run(["cmake", "--build", bdir])
    built = os.path.join(bdir, "OrbitDuel.bin")
    # rename to "Orbit Duel" (the name in the Finder and in the boot blocks)
    h, d, r = rsrc.read_macbinary(built)
    h = bytearray(h)
    h[1] = len(APP_NAME)
    h[2:65] = APP_NAME + b"\0" * (63 - len(APP_NAME))
    named = os.path.join(B, "named.bin")
    rsrc.write_macbinary(named, bytes(h), d, r)
    app = os.path.join(B, "Orbit Duel.bin")
    args = [sys.executable, "-I", os.path.join(ROOT, "toolchain", "icons", "add_icon.py"), named, "--out", app]
    args += ["--icon", ICON] if os.path.exists(ICON) else []
    if os.path.exists(ICON):
        run(args)
    else:
        os.replace(named, app)
    size = os.path.getsize(app)
    print(f"app: {app} ({size:,} bytes MacBinary, resource fork {len(r):,})")
    if a.no_disks:
        return
    plus = os.path.join(B, "orbitduel.img")
    print(run([sys.executable, "-I", os.path.join(ROOT, "toolchain", "template", "build_disk.py"), "--app", app,
               "--out", plus, "--label", "Orbit Duel", "--keep-se"]).strip().splitlines()[-1])
    se = os.path.join(B, "orbitduel_se.img")
    print(run([sys.executable, "-I", os.path.join(HERE, "tools", "make_se_floppy.py"), "--app", app, "--out", se,
               "--readme", os.path.join(HERE, "SETUP.md")]).strip().splitlines()[-1])
    hd = os.path.join(B, "orbitduel_se7.hd")
    run([sys.executable, "-I", os.path.join(ROOT, "toolchain", "se7", "build_se_disk.py"), "--app", app, "--out", hd])
    print(f"SE System 7 test disk: {hd}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Guess My Doodle's floppies.

    make_disks.py [--model app/data/Guess.gmdl] [--app build/app/GuessMyDoodle.bin] [--out build/release]

  guess_plus.img   800K, boots a Mac Plus (1 MB is enough) straight into the game: trimmed System
                   6.0 with the SE's ROM patch (so it starts an SE too), Narrow Screen, the game,
                   "Guess Model" and the Read Me (toolchain/template/build_disk.py).
  guess_se.img     1.44 MB for the SE's SuperDrive: System 6.0.7 + Finder, the same files; starts
                   straight into the game, quitting goes to the Finder (the shared recipe of
                   dream/app/make_se_floppy.py).
The app gets its Finder icon and bundle from toolchain/icons/add_icon.py (art in
toolchain/icons/art/guess*.txt). hfsutils always runs with a private HOME (AGENTS.md).
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "mactools"))
import rsrc  # noqa: E402

APP_NAME = b"Guess My Doodle"
CREATOR = b"GsDd"
TOOLS607 = os.path.join(ROOT, "emu", "disks", "system-6.0.7-tools.dc42")
SE_SIZE = 1474560
STARTUP_NAME = 90

README = """Guess My Doodle
================

Draw something with the mouse, and the Mac guesses what it is.

The Mac names a thing (a bicycle, a cactus, the Eiffel Tower...) and you have 20 seconds to draw it in
the box. While you draw, the Mac keeps guessing: after every stroke, and every second or so while you
are still drawing. When its guess is right, the round ends early. A game has six rounds; at the end
you see your six drawings with the Mac's three best guesses for each.

Controls
  Mouse          draw in the box; click the buttons (Undo, Clear, Skip, Next)
  Return         start / go / next
  Command-Z      undo the last stroke (also Delete)
  Command-K      clear the drawing
  M              sound on/off
  Esc            back to the title; at the title, quit (the Plus keyboard's ` key)
  File > Quit    quit

If the MacinTalk file is in the System Folder (or next to the game), the Mac also says its guesses
out loud (File > Speak Guesses).

How it works
  The guesses come from a small neural network: a convolutional classifier whose weights are all
  -1, 0 or +1. When the game starts, it turns the network into 68000 machine code (a zero weight
  costs nothing, a +1 is one add, a -1 one subtract), so a guess takes about a second on a real
  Mac Plus. Your drawing is scaled to fit a 32 x 32 grid of dots, the network's input.
  It knows 150 things, the ones it recognises best among the 345 of Quick, Draw!.

Credits
  The network was trained on the Quick, Draw! dataset by Google
  (https://quickdraw.withgoogle.com/data, https://github.com/googlecreativelab/quickdraw-dataset),
  licensed under Creative Commons Attribution 4.0 International (CC BY 4.0,
  https://creativecommons.org/licenses/by/4.0/). The drawings were rescaled and rasterised for
  training; the network's weights are derived from them.
  Code, sounds and art: original, MIT licence (github.com/devon7y/mac-plus-ai).
"""


def run(cmd, env=None, quiet=False):
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"failed: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    if not quiet and r.stdout.strip():
        print(r.stdout.rstrip())
    return r.stdout


def macbinary(data, out, name, ftype, creator):
    hdr = bytearray(128)
    hdr[1] = len(name)
    hdr[2:2 + len(name)] = name
    hdr[65:69], hdr[69:73] = ftype, creator
    rsrc.write_macbinary(out, bytes(hdr), data, b"")


def prepare(a, work):
    """The app (renamed, with its icon), the model and the Read Me as MacBinary files."""
    h, d, r = rsrc.read_macbinary(a.app)
    h = bytearray(h)
    h[1] = len(APP_NAME)
    h[2:65] = APP_NAME + b"\0" * (63 - len(APP_NAME))
    app = os.path.join(work, "app.bin")
    rsrc.write_macbinary(app, bytes(h), d, r)
    art = os.path.join(ROOT, "toolchain", "icons", "art")
    run([sys.executable, "-I", os.path.join(ROOT, "toolchain", "icons", "add_icon.py"), app,
         "--icon", os.path.join(art, "guess.txt"), "--doc", "GMDL=" + os.path.join(art, "guess_model.txt")],
        quiet=True)
    model = os.path.join(work, "model.bin")
    macbinary(open(a.model, "rb").read(), model, b"Guess Model", b"GMDL", CREATOR)
    readme = os.path.join(work, "readme.bin")
    macbinary(README.encode("mac_roman").replace(b"\n", b"\r"), readme, b"Guess My Doodle Read Me", b"TEXT", b"ttxt")
    return app, model, readme


def plus_disk(a, app, model, readme):
    out = os.path.join(a.out, "guess_plus.img")
    run([sys.executable, "-I", os.path.join(ROOT, "toolchain", "template", "build_disk.py"), "--app", app,
         "--out", out, "--label", "Guess My Doodle", "--keep-se", "--extra", model, readme])
    return out


def se_disk(a, app, model, readme, work):
    """(a.macintalk: test builds only.)"""
    env = dict(os.environ, HOME=tempfile.mkdtemp(prefix="hfs_home_"))
    d = open(TOOLS607, "rb").read()
    raw = os.path.join(work, "tools607.img")
    open(raw, "wb").write(d[84:84 + struct.unpack(">I", d[0x40:0x44])[0]])     # DiskCopy 4.2 -> raw
    boot = bytearray(open(raw, "rb").read(1024))
    if boot[:2] != b"LK":
        sys.exit("the 6.0.7 tools disk has no boot blocks")
    run(["hmount", raw], env, quiet=True)
    try:
        for f in ("System", "Finder"):
            run(["hcopy", "-m", f":System Folder:{f}", os.path.join(work, f + ".bin")], env)
    finally:
        subprocess.run(["humount"], env=env, capture_output=True)
    out = os.path.join(a.out, "guess_se.img")
    open(out, "wb").write(bytes(SE_SIZE))
    run(["hformat", "-l", "Guess My Doodle", out], env, quiet=True)
    try:
        run(["hmkdir", ":System Folder"], env)
        run(["hcopy", "-m", os.path.join(work, "System.bin"), ":System Folder:System"], env)
        run(["hcopy", "-m", os.path.join(work, "Finder.bin"), ":System Folder:Finder"], env)
        for f in (app, model, readme) + ((a.macintalk,) if a.macintalk else ()):
            run(["hcopy", "-m", f, ":System Folder:"], env)
        run(["hattrib", "-b", ":System Folder"], env)
        listing = run(["hls", "-l", ":System Folder"], env, quiet=True)
        vol = run(["hvol"], env, quiet=True)
    finally:
        subprocess.run(["humount"], env=env, capture_output=True)
    boot[STARTUP_NAME:STARTUP_NAME + 16] = bytes([len(APP_NAME)]) + APP_NAME + b" " * (15 - len(APP_NAME))
    img = bytearray(open(out, "rb").read())
    img[:1024] = boot
    open(out, "wb").write(img)
    print(listing.rstrip())
    print([l for l in vol.splitlines() if "free" in l][0].strip())
    print(f"startup app: {APP_NAME.decode()}  ->  {out} ({SE_SIZE:,} bytes)")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default=os.path.join(HERE, "data", "Guess.gmdl"))
    ap.add_argument("--app", default=os.path.join(ROOT, "guess", "build", "app", "GuessMyDoodle.bin"))
    ap.add_argument("--out", default=os.path.join(ROOT, "guess", "build", "release"))
    ap.add_argument("--only", choices=("plus", "se"), default=None)
    ap.add_argument("--macintalk", default="", help="test builds only: put this MacinTalk (MacBinary) in the "
                    "System Folder (Apple software: never on the release floppies)")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="guess_disks_", dir=os.path.join(ROOT, "guess", "build"))
    app, model, readme = prepare(a, work)
    if a.only != "se":
        plus_disk(a, app, model, readme)
    if a.only != "plus":
        se_disk(a, app, model, readme, work)


if __name__ == "__main__":
    main()

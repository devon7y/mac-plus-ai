#!/usr/bin/env python3
"""Build Ask AI: the app, its Finder icon and the boot floppies.

    python3 -I askai/build.py                    app + both release floppies
    python3 -I askai/build.py --test-dial 127.0.0.1:16800 [--test-pin 1234]
                                                  also a test floppy whose prefs dial that address
                                                  and connect at startup (for the MAME check)

Outputs (askai/build/, gitignored):
  app/AskAI.bin                 the Retro68 build
  release/Ask AI.bin            the app named "Ask AI", with its icon (toolchain/icons/art/askai.txt)
  release/askai_plus.img        800K Plus boot floppy: trimmed System 6.0 + Narrow Screen, starts Ask AI
  release/askai_se.img          1.44 MB SE boot floppy: System 6.0.7 System + Finder, starts Ask AI
                                (quitting goes to the Finder)
  test/askai_test.img           (--test-dial) the Plus floppy with "Ask AI Prefs" preset

The prefs layout matches the Prefs struct in app/link.h. hfsutils runs with a private HOME (AGENTS.md).
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(HERE, "build")
TOOLS = os.path.join(ROOT, "mactools")
sys.path.insert(0, TOOLS)
import rsrc  # noqa: E402

TOOLCHAIN = os.path.expanduser("~/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake")
TOOLS607 = os.path.join(ROOT, "emu", "disks", "system-6.0.7-tools.dc42")
APP_NAME = b"Ask AI"
SE_SIZE = 1474560
STARTUP_NAME = 90                      # offset of the startup application's name in the boot blocks

DEFAULT_SYSTEM = ("Answer in short plain text for a 1986 Mac with a 512x342 black-and-white screen: no "
                  "tables, no emoji, short paragraphs, under 150 words unless I ask for more.")


def run(cmd, env=None, quiet=True):
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit("failed: %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))
    if not quiet and r.stdout.strip():
        print(r.stdout.rstrip())
    return r.stdout


def pstr(s, size):
    b = s.encode("mac_roman")[:size - 1]
    return bytes([len(b)]) + b + bytes(size - 1 - len(b))


def prefs_bytes(dial="", init="ATE0V1", pin="", model="", system=DEFAULT_SYSTEM, baud=3, flow=0, port=0,
                auto=1):
    """The Prefs struct of app/link.h (464 bytes, big-endian)."""
    b = struct.pack(">Ihhhhhh", 0x41736B50, 1, baud, flow, port, auto, 0)
    b += pstr(dial, 64) + pstr(init, 64) + pstr(pin, 32) + pstr(model, 32) + pstr(system, 256)
    assert len(b) == 464
    return b


def macbinary_file(data, out, name, ftype, creator):
    hdr = bytearray(128)
    hdr[1] = len(name)
    hdr[2:2 + len(name)] = name
    hdr[65:69], hdr[69:73] = ftype, creator
    rsrc.write_macbinary(out, bytes(hdr), data, b"")


def build_app():
    appdir = os.path.join(BUILD, "app")
    if not os.path.exists(os.path.join(appdir, "build.ninja")):
        run(["cmake", "-S", os.path.join(HERE, "app"), "-B", appdir, "-G", "Ninja",
             "-DCMAKE_TOOLCHAIN_FILE=" + TOOLCHAIN])
    run(["cmake", "--build", appdir])
    rel = os.path.join(BUILD, "release")
    os.makedirs(rel, exist_ok=True)
    h, d, r = rsrc.read_macbinary(os.path.join(appdir, "AskAI.bin"))
    h = bytearray(h)
    h[1] = len(APP_NAME)
    h[2:65] = APP_NAME + bytes(63 - len(APP_NAME))
    named = os.path.join(BUILD, "app", "AskAI_named.bin")
    rsrc.write_macbinary(named, bytes(h), d, r)
    out = os.path.join(rel, "Ask AI.bin")
    run([sys.executable, "-I", os.path.join(ROOT, "toolchain", "icons", "add_icon.py"), named,
         "--icon", os.path.join(ROOT, "toolchain", "icons", "art", "askai.txt"), "--out", out])
    print("app: %s (%d bytes)" % (out, os.path.getsize(out)))
    return out


def plus_floppy(app, out, extra=()):
    os.makedirs(os.path.dirname(out), exist_ok=True)
    cmd = [sys.executable, "-I", os.path.join(ROOT, "toolchain", "template", "build_disk.py"), "--app", app,
           "--out", out, "--label", "Ask AI"]
    if extra:
        cmd += ["--extra"] + list(extra)
    txt = run(cmd)
    free = [ln for ln in txt.splitlines() if "free" in ln]
    print("Plus floppy: %s  (%s)" % (out, free[0].strip() if free else ""))


def se_floppy(app, out, extra=()):
    """1.44 MB System 6.0.7 SE boot floppy (the recipe of dream/app/make_se_floppy.py)."""
    work = os.path.join(BUILD, "se_work")             # reused (overwritten) by every build
    os.makedirs(work, exist_ok=True)
    env = dict(os.environ, HOME=tempfile.mkdtemp(prefix="hfs_home_"))   # private: see AGENTS.md
    d = open(TOOLS607, "rb").read()
    raw = os.path.join(work, "tools607.img")
    open(raw, "wb").write(d[84:84 + struct.unpack(">I", d[0x40:0x44])[0]])
    boot = bytearray(open(raw, "rb").read(1024))
    if boot[:2] != b"LK":
        sys.exit("the 6.0.7 tools disk has no boot blocks")
    run(["hmount", raw], env)
    try:
        for f in ("System", "Finder"):
            run(["hcopy", "-m", ":System Folder:" + f, os.path.join(work, f + ".bin")], env)
    finally:
        subprocess.run(["humount"], env=env, capture_output=True)
    open(out, "wb").write(bytes(SE_SIZE))
    run(["hformat", "-l", "Ask AI SE", out], env)
    try:
        run(["hmkdir", ":System Folder"], env)
        run(["hcopy", "-m", os.path.join(work, "System.bin"), ":System Folder:System"], env)
        run(["hcopy", "-m", os.path.join(work, "Finder.bin"), ":System Folder:Finder"], env)
        run(["hcopy", "-m", app, ":System Folder:"], env)
        for f in extra:
            run(["hcopy", "-m", f, ":System Folder:"], env)
        run(["hattrib", "-b", ":System Folder"], env)
        vol = run(["hvol"], env)
    finally:
        subprocess.run(["humount"], env=env, capture_output=True)
    boot[STARTUP_NAME:STARTUP_NAME + 16] = bytes([len(APP_NAME)]) + APP_NAME + b" " * (15 - len(APP_NAME))
    img = bytearray(open(out, "rb").read())
    img[:1024] = boot
    open(out, "wb").write(img)
    free = [ln for ln in vol.splitlines() if "free" in ln]
    print("SE floppy: %s  (%s)" % (out, free[0].strip() if free else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--test-dial", help="also build test/askai_test.img with prefs dialling this address")
    ap.add_argument("--test-pin", default="")
    ap.add_argument("--test-baud", type=int, default=6,
                    help="index: 0 1200 ... 3 19200 ... 5 57600, 6 the MAME test speed (7200 in MAME)")
    ap.add_argument("--no-release", action="store_true", help="skip the release floppies")
    a = ap.parse_args()
    app = build_app()
    rel = os.path.join(BUILD, "release")
    if not a.no_release:
        plus_floppy(app, os.path.join(rel, "askai_plus.img"))
        se_floppy(app, os.path.join(rel, "askai_se.img"))
    if a.test_dial:
        tdir = os.path.join(BUILD, "test")
        os.makedirs(tdir, exist_ok=True)
        pf = os.path.join(tdir, "AskAIPrefs.bin")
        macbinary_file(prefs_bytes(dial=a.test_dial, pin=a.test_pin, baud=a.test_baud), pf, b"Ask AI Prefs",
                       b"pref", b"AskA")
        plus_floppy(app, os.path.join(tdir, "askai_test.img"), [pf])


if __name__ == "__main__":
    main()

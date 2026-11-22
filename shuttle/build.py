#!/usr/bin/env python3
"""Build Shuttle, its icon, and the starter floppies.

    python3 -I shuttle/build.py                 everything below
    python3 -I shuttle/build.py --test-only     only the app and the MAME test floppy

Outputs (shuttle/build/, gitignored):
  app/Shuttle.bin              the Retro68 build
  release/Shuttle.bin          the app with its Finder icon (toolchain/icons/art/shuttle.txt)
  release/Ask AI.bin           Ask AI, built from askai/app (read-only, compiled here) with its icon
  plus_modem_800k.img          800K boot floppy for the Plus: System 6.0 (trimmed, Finder kept, also boots
                               an SE) + Narrow Screen, Shuttle and Ask AI at the top with their prefs
                               preset (Shuttle dials "mac", Ask AI dials "ai", both 19200), TeachText,
                               "Read Me". Starts at the Finder.
  Plus Modem.image             the same disk as a Disk Copy 4.2 image (dImg/dCpy, zero tags)
  starter_144.img              1.44 MB HFS disk for a Mac SE FDHD: Disk Copy 4.2, Plus Modem.image,
                               "Read Me First", Shuttle, Ask AI, TeachText
  test/shuttle_test.img        MAME test floppy: boots straight into Shuttle, prefs dial "mac" at the
                               hidden MAME test speed and connect at startup; "Upload Test" at the top
  test/shuttle_se7.hd          (--test-se7) the same on the System 7.0.1 SE testbed hard disk

Disk Copy 4.2 comes from build/vendor/Disk_Copy_(v4.2).sit (Macintosh Repository, see README.md);
its SHA-256 is checked. hfsutils always runs with a private HOME (AGENTS.md).
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(HERE, "build")
sys.path.insert(0, os.path.join(ROOT, "mactools"))
sys.path.insert(0, os.path.join(ROOT, "toolchain", "icons"))
sys.path.insert(0, os.path.join(HERE, "server"))
import rsrc  # noqa: E402
import add_icon  # noqa: E402
import shuttle_server as srv  # noqa: E402  (MacBinary, Disk Copy 4.2 and text helpers)

TOOLCHAIN = os.path.expanduser("~/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake")
SYSTOOLS = os.path.join(ROOT, "emu", "disks", "system-6.0", "6.0 System Tools.dc42")
NARROW = os.path.join(ROOT, "narrowscreen", "build", "Narrow Screen.bin")
DC_ARCHIVE = os.path.join(BUILD, "vendor", "Disk_Copy_(v4.2).sit")
DC_SHA256 = "2f62f359df75301dca0074af072e6e2b09f487b4ee91c9de7ab10e986aaa7424"
DC_URL = "https://www.macintoshrepository.org/2416-diskcopy-4-2-5-0-5-5-6-0-6-2-6-3-3-6-4-6-5b13-7-0-8-0"
ASKAI_SYSTEM = ("Answer in short plain text for a 1986 Mac with a 512x342 black-and-white screen: no "
                "tables, no emoji, short paragraphs, under 150 words unless I ask for more.")
HFS_ENV = dict(os.environ, HOME=tempfile.mkdtemp(prefix="hfs_home_"))


def run(cmd, env=None, quiet=True):
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit("failed: %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))
    if not quiet and r.stdout.strip():
        print(r.stdout.rstrip())
    return r.stdout


def hfs(*cmd):
    return run(list(cmd), HFS_ENV)


def pstr(s, size):
    b = s.encode("mac_roman")[:size - 1]
    return bytes([len(b)]) + b + bytes(size - 1 - len(b))


def shuttle_prefs(dial="mac", init="ATE0V1", baud=3, flow=0, port=0, auto=0, window=0, block=0):
    """The Prefs struct of app/link.h (148 bytes, big-endian)."""
    b = struct.pack(">Ihhhhhhhh", 0x53687450, 1, baud, flow, port, auto, window, block, 0)
    b += pstr(dial, 64) + pstr(init, 64)
    assert len(b) == 148
    return b


def askai_prefs(dial="", init="ATE0V1", baud=3, auto=1):
    """Ask AI's Prefs struct (askai/app/link.h, 464 bytes): as askai/build.py writes it."""
    b = struct.pack(">Ihhhhhh", 0x41736B50, 1, baud, 0, 0, auto, 0)
    b += pstr(dial, 64) + pstr(init, 64) + pstr("", 32) + pstr("", 32) + pstr(ASKAI_SYSTEM, 256)
    assert len(b) == 464
    return b


def mb_file(path, name, ftype, creator, data=b"", rsrc_fork=b"", flags=0):
    open(path, "wb").write(srv.macbinary(name.encode("mac_roman"), ftype, creator, flags, data, rsrc_fork))
    return path


def renamed(src, dst, name):
    h, d, r = rsrc.read_macbinary(src)
    h = bytearray(h)
    h[1] = len(name)
    h[2:65] = name + bytes(63 - len(name))
    rsrc.write_macbinary(dst, bytes(h), d, r)
    return dst


# ------------------------------------------------------------------------------------------ apps
def build_cmake(src, out):
    if not os.path.exists(os.path.join(out, "build.ninja")):
        run(["cmake", "-S", src, "-B", out, "-G", "Ninja", "-DCMAKE_TOOLCHAIN_FILE=" + TOOLCHAIN])
    run(["cmake", "--build", out])


def build_apps():
    rel = os.path.join(BUILD, "release")
    os.makedirs(rel, exist_ok=True)
    build_cmake(os.path.join(HERE, "app"), os.path.join(BUILD, "app"))
    shuttle = os.path.join(rel, "Shuttle.bin")
    add_icon.apply(os.path.join(BUILD, "app", "Shuttle.bin"), out=shuttle,
                   icon=os.path.join(ROOT, "toolchain", "icons", "art", "shuttle.txt"), quiet=True)
    build_cmake(os.path.join(ROOT, "askai", "app"), os.path.join(BUILD, "askai_app"))
    named = renamed(os.path.join(BUILD, "askai_app", "AskAI.bin"), os.path.join(BUILD, "askai_app", "AskAI_named.bin"),
                    b"Ask AI")
    askai = os.path.join(rel, "Ask AI.bin")
    add_icon.apply(named, out=askai, icon=os.path.join(ROOT, "toolchain", "icons", "art", "askai.txt"), quiet=True)
    print("apps: Shuttle %d bytes, Ask AI %d bytes (MacBinary)" % (os.path.getsize(shuttle), os.path.getsize(askai)))
    return shuttle, askai


def disk_copy():
    """Disk Copy 4.2 as MacBinary, from the Macintosh Repository archive (checked by SHA-256)."""
    if not os.path.exists(DC_ARCHIVE):
        sys.exit("Disk Copy 4.2 is missing: download Disk_Copy_(v4.2).sit from\n  %s\n(the 15.3 KB entry, SHA-1 "
                 "8fd1b1fa...) to %s" % (DC_URL, DC_ARCHIVE))
    digest = hashlib.sha256(open(DC_ARCHIVE, "rb").read()).hexdigest()
    if digest != DC_SHA256:
        sys.exit("%s: SHA-256 %s, expected %s" % (DC_ARCHIVE, digest, DC_SHA256))
    out = os.path.join(BUILD, "vendor", "unpacked")
    app = os.path.join(out, "Disk Copy (v4.2)", "Disk Copy 4.2")
    if not os.path.exists(app):
        os.makedirs(out, exist_ok=True)
        run(["unar", "-q", "-f", "-k", "fork", "-nq", "-o", out, DC_ARCHIVE])
    fi = srv.get_finder_info(app)
    rf = srv.read_rsrc(app)
    if not fi or fi[:8] != b"APPLdCpy" or b"4.2, Copyright" not in rf:
        sys.exit("unexpected contents in " + DC_ARCHIVE)
    path = os.path.join(BUILD, "vendor", "Disk Copy 4.2.bin")
    open(path, "wb").write(srv.macbinary(b"Disk Copy 4.2", b"APPL", b"dCpy", (fi[8] << 8 | fi[9]) & ~0x0100,
                                         open(app, "rb").read(), rf))
    return path


# ------------------------------------------------------------------------------------------ disks
def system_tools():
    """The raw 6.0 System Tools disk and MacBinary copies of System, Finder and TeachText from it."""
    work = os.path.join(BUILD, "disk_work")
    os.makedirs(work, exist_ok=True)
    raw = os.path.join(work, "systools.img")
    d = open(SYSTOOLS, "rb").read()
    open(raw, "wb").write(d[84:84 + struct.unpack(">I", d[0x40:0x44])[0]])
    hfs("hmount", raw)
    try:
        for src, dst in ((":System Folder:System", "System.bin"), (":System Folder:Finder", "Finder.bin"),
                         (":TeachText", "TeachText.bin")):
            hfs("hcopy", "-m", src, os.path.join(work, dst))
    finally:
        subprocess.run(["humount"], env=HFS_ENV, capture_output=True)
    return raw, work


def text_file(path, src, name, free=None):
    text = open(src, encoding="utf-8").read()
    if free is not None:
        text = text.replace("{FREE}", str(free))
    return mb_file(path, name, b"TEXT", b"ttxt", srv.text_to_mac(text.encode("utf-8")))


def hvol_free():
    vol = hfs("hvol")
    line = [ln for ln in vol.splitlines() if "free" in ln][0]
    return int(line.split("bytes free")[0].split()[-1]) if "bytes free" in line else line


def build_plus_floppy(shuttle, askai, free_guess=None):
    raw, work = system_tools()
    sys_trim = os.path.join(work, "System-safe-se.bin")
    run([sys.executable, "-I", os.path.join(ROOT, "mactools", "trim_system.py"),
         os.path.join(work, "System.bin"), sys_trim, "--level", "safe", "--keep-se"])
    sp = mb_file(os.path.join(work, "ShuttlePrefs.bin"), "Shuttle Prefs", b"pref", b"Shtl", shuttle_prefs())
    ap = mb_file(os.path.join(work, "AskAIPrefs.bin"), "Ask AI Prefs", b"pref", b"AskA", askai_prefs(dial="ai"))
    out = os.path.join(BUILD, "plus_modem_800k.img")
    readme = os.path.join(work, "ReadMe.bin")
    for attempt in range(2):
        text_file(readme, os.path.join(HERE, "disk", "Read Me.txt"), "Read Me", free_guess or 999)
        open(out, "wb").write(bytes(819200))
        hfs("hformat", "-l", "Plus Modem", out)
        try:
            hfs("hmkdir", ":System Folder")
            hfs("hcopy", "-m", sys_trim, ":System Folder:System")
            hfs("hcopy", "-m", os.path.join(work, "Finder.bin"), ":System Folder:Finder")
            hfs("hcopy", "-m", NARROW, ":System Folder:")
            hfs("hcopy", "-m", sp, ":System Folder:")
            hfs("hcopy", "-m", ap, ":System Folder:")
            hfs("hattrib", "-b", ":System Folder")
            hfs("hcopy", "-m", shuttle, ":")
            hfs("hcopy", "-m", askai, ":")
            hfs("hcopy", "-m", os.path.join(work, "TeachText.bin"), ":")
            hfs("hcopy", "-m", readme, ":")
            free = hvol_free()
            listing = hfs("hls", "-lR", ":")
        finally:
            subprocess.run(["humount"], env=HFS_ENV, capture_output=True)
        want = free // 1024 // 10 * 10
        if free_guess == want:
            break
        free_guess = want
    img = bytearray(open(out, "rb").read())
    img[:1024] = open(raw, "rb").read(1024)            # System 6.0 boot blocks: System, Finder
    open(out, "wb").write(img)
    print(listing.rstrip())
    print("plus_modem_800k.img: %d bytes free (Read Me says about %dK)" % (free, free_guess))
    image = os.path.join(BUILD, "Plus Modem.image")
    open(image, "wb").write(srv.dc42(bytes(img), b"Plus Modem"))
    print("Plus Modem.image: Disk Copy 4.2, %d bytes" % os.path.getsize(image))
    return out, image, free


def build_starter(shuttle, askai, image, dcopy):
    work = os.path.join(BUILD, "disk_work")
    out = os.path.join(BUILD, "starter_144.img")
    img_mb = os.path.join(work, "PlusModemImage.bin")
    mb_file(img_mb, "Plus Modem.image", b"dImg", b"dCpy", open(image, "rb").read())
    readme = text_file(os.path.join(work, "ReadMeFirst.bin"), os.path.join(HERE, "disk", "Read Me First.txt"),
                       "Read Me First")
    open(out, "wb").write(bytes(1474560))
    hfs("hformat", "-l", "Shuttle Starter", out)
    try:
        for f in (readme, dcopy, img_mb, shuttle, askai, os.path.join(work, "TeachText.bin")):
            hfs("hcopy", "-m", f, ":")
        free = hvol_free()
        listing = hfs("hls", "-l", ":")
    finally:
        subprocess.run(["humount"], env=HFS_ENV, capture_output=True)
    print(listing.rstrip())
    print("starter_144.img: %d bytes free" % free)
    return out


def build_test_floppy(shuttle):
    tdir = os.path.join(BUILD, "test")
    os.makedirs(tdir, exist_ok=True)
    pf = mb_file(os.path.join(tdir, "ShuttlePrefs.bin"), "Shuttle Prefs", b"pref", b"Shtl",
                 shuttle_prefs(dial="mac", baud=6, auto=1))
    out = os.path.join(tdir, "shuttle_test.img")
    run([sys.executable, "-I", os.path.join(ROOT, "toolchain", "template", "build_disk.py"), "--app", shuttle,
         "--out", out, "--label", "Shuttle Test", "--extra", pf])
    up = mb_file(os.path.join(tdir, "UploadTest.bin"), "Upload Test", b"TEXT", b"ttxt",
                 b"Hello from the Plus!\r" * 150, bytes(range(256)) * 20)
    hfs("hmount", out)
    try:
        hfs("hcopy", "-m", up, ":")
        free = hvol_free()
    finally:
        subprocess.run(["humount"], env=HFS_ENV, capture_output=True)
    print("test floppy: %s (%d bytes free)" % (out, free))
    return out


def build_test_se7(shuttle):
    """The SE / System 7.0.1 testbed hard disk: Shuttle in Startup Items, its test prefs, "Upload Test"."""
    tdir = os.path.join(BUILD, "test")
    out = os.path.join(tdir, "shuttle_se7.hd")
    up = os.path.join(tdir, "UploadTest.bin")
    run([sys.executable, "-I", os.path.join(ROOT, "toolchain", "se7", "build_se_disk.py"), "--app", shuttle,
         "--out", out, "--extra", up])
    hfs("hmount", out, "1")
    try:
        hfs("hcopy", "-m", os.path.join(tdir, "ShuttlePrefs.bin"), ":System Folder:")
    finally:
        subprocess.run(["humount"], env=HFS_ENV, capture_output=True)
    print("SE test disk: %s" % out)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--test-only", action="store_true", help="only the app and the MAME test floppy")
    ap.add_argument("--test-se7", action="store_true", help="also the SE / System 7.0.1 test hard disk")
    a = ap.parse_args()
    shuttle, askai = build_apps()
    build_test_floppy(shuttle)
    if a.test_se7:
        build_test_se7(shuttle)
    if a.test_only:
        return
    _, image, _ = build_plus_floppy(shuttle, askai)
    build_starter(shuttle, askai, image, disk_copy())


if __name__ == "__main__":
    main()

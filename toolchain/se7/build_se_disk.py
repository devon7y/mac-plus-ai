#!/usr/bin/env python3
"""Make a SCSI hard-disk image for the emulated SE (macsefd, System 7.0.1) with an application.

    build_se_disk.py --app build/MyGame.bin --out build/mygame_se7.hd [--shell] [--extra F.bin ...]

Starts from the clean System 7.0.1 install emu/disks/system-7.0.1/se7_base.hd (made by
install_system.sh) and copies the MacBinary application into ":System Folder:Startup Items:",
so the Finder launches it after startup, as on the real SE. With --shell the app goes into the
System Folder instead and the boot blocks name it as the startup application and shell: no
Finder, slightly more free memory. --extra copies more MacBinary files to the volume's top
level. Run it with:

    MACHINE=macsefd RAM=4M HARDDISK=build/mygame_se7.hd emu/run_test.sh none script.lua 250

hfsutils runs with a private $HOME (see AGENTS.md).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BASE = os.path.join(ROOT, "emu", "disks", "system-7.0.1", "se7_base.hd")
PART_START = 96 * 512                    # the HFS partition starts at block 96 (see make_blank_hd.py)
SHELL_NAME, STARTUP_NAME = 26, 90        # Str15 fields in the boot blocks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--base", default=BASE)
    ap.add_argument("--shell", action="store_true", help="no Finder: the app is the startup app and shell")
    ap.add_argument("--extra", nargs="*", default=[])
    a = ap.parse_args()
    if not os.path.exists(a.base):
        sys.exit(f"{a.base} not found: run toolchain/se7/install_system.sh {a.base}")
    shutil.copyfile(a.base, a.out)
    os.chmod(a.out, 0o644)
    hdr = open(a.app, "rb").read(128)
    name = hdr[2:2 + hdr[1]]
    home = tempfile.mkdtemp(prefix="hfs_home_")
    env = dict(os.environ, HOME=home)

    def hfs(*cmd):
        r = subprocess.run(cmd, env=env, capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"failed: {' '.join(cmd)}\n{r.stderr}")
        return r.stdout
    try:
        hfs("hmount", a.out, "1")
        try:
            if a.shell:
                hfs("hcopy", "-m", a.app, ":System Folder:")
            else:
                if "Startup Items" not in hfs("hls", ":System Folder"):
                    hfs("hmkdir", ":System Folder:Startup Items")
                hfs("hcopy", "-m", a.app, ":System Folder:Startup Items:")
            for f in a.extra:
                hfs("hcopy", "-m", f, ":")
            where = ":System Folder:" if a.shell else ":System Folder:Startup Items"
            print(hfs("hls", "-l", where).rstrip())
            print([l for l in hfs("hvol").splitlines() if "free" in l][0].strip())
        finally:
            subprocess.run(["humount"], env=env, capture_output=True)
    finally:
        shutil.rmtree(home, ignore_errors=True)
    if a.shell:
        if len(name) > 15:
            sys.exit("app name too long for the boot blocks (max 15)")
        img = bytearray(open(a.out, "rb").read())
        bb = PART_START
        if img[bb:bb + 2] != b"LK":
            sys.exit("no boot blocks in the HFS partition")
        field = bytes([len(name)]) + name + b" " * (15 - len(name))
        img[bb + SHELL_NAME:bb + SHELL_NAME + 16] = field
        img[bb + STARTUP_NAME:bb + STARTUP_NAME + 16] = field
        open(a.out, "wb").write(img)
    print(f"{name.decode('mac_roman')} -> {a.out} ({'shell, no Finder' if a.shell else 'Startup Items, with Finder'})")


if __name__ == "__main__":
    main()

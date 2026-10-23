#!/usr/bin/env python3
"""Build the public web disk: a bootable raw HFS volume for Mini vMac (no partition map).

    python3 -I web/build_disk.py [--size-mb 16] [--out web/build/disk/hd_unbaked.dsk]

Then bake it (first boot in the browser emulator: the Finder builds its Desktop file, i.e. the
apps' icons) with   node web/tools/bake.mjs web/build/disk/hd_unbaked.dsk web/build/disk/hd.dsk
(web/build.sh runs every step.)

Where things come from (edit APPS below to swap in new builds; nothing is a frozen copy):
  * System Folder: the System 6.0 System Folder of the all-games demo disk (SYSTEM_FROM), which
    has the Talking Moose installed (DRVR in the System, MacinTalk and Moose Phrases beside it).
  * Our apps: the projects' current build outputs, through their emu/games.json entries (the boot
    floppy names the app as its startup application; the app's data files are the non-system
    files beside it plus every file on the entry's second floppy), or explicit MacBinary files.
  * Glider 4.06 and its files: MacBinary files in toolchain/build/demo/extras.

Allowed on this disk (Devon, 2026-10-20): Endurance, Rail Runner, Blockworld, Glider 4.06, Doodle,
Pixel (Plus version), Dream, and the Talking Moose. Never: Pokemon, Super Mario (1 and 3),
Plants vs. Zombies, Narrow Screen. The script refuses to finish if any of their names or creator
codes turn up on the new volume.

Layout (Devon, 2026-10-20: every app directly on the Finder desktop):
  * The apps and a Read Me are in the volume's root directory, marked as desktop items (Finder
    flag isOnDesk; System 6 keeps desktop items in the root directory) at fixed positions: two
    columns on the left, clear of the disk icon (top right) and the Trash (bottom right).
  * Their data files sit beside them in the root directory (each app looks in its own folder or
    the default folder; both are the root here). Data files are invisible unless listed in
    VISIBLE_DATA (things a person opens by hand: Glider's houses and history, Dream's licence);
    those are in the disk window.
  * hasBeenInited is cleared so the Finder registers each app's icons on the first boot.

The Finder info edits are made directly in the catalog B-tree (hfsutils can't set icon positions
or Finder flags). The boot blocks come from the demo disk (System Tools boot blocks, 128 KB system
heap). hfsutils keeps its current volume in $HOME/.hcwd, so every call gets a private HOME.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GAMES_JSON = os.path.join(ROOT, "emu", "games.json")
SYSTEM_FROM = os.path.join(ROOT, "toolchain", "build", "demo", "demo.hd")   # Apple-partitioned
EXTRAS = os.path.join(ROOT, "toolchain", "build", "demo", "extras")

# The apps, in desktop order (first column top to bottom, then the second column).
#   "entry":     an emu/games.json entry (the project's current build output)
#   "macbinary": explicit MacBinary files, the app first (paths relative to the repo)
APPS = [
    {"name": "Endurance",   "entry": "racer"},
    {"name": "Rail Runner", "entry": "runner"},
    # Blockworld M7: the game and its locked ready-made starting world, side by side in the same folder
    # (the root, where desktop items live); the world file stays hidden. "icon": a toolchain/icons
    # preset applied to our copy of the app (idempotent; the M7 build already carries its icon).
    {"name": "Blockworld",  "macbinary": ["blockworld/build/release/Blockworld.bin",
                                          "blockworld/build/release/Blockworld Default.bin"], "icon": "blockworld"},
    {"name": "Glider 4.06", "macbinary": [f"toolchain/build/demo/extras/{f}.bin" for f in
                                          ("Glider_4.06_bw", "B_W_Art", "The_House", "The_House_2", "Glider_History")]},
    {"name": "Doodle",      "entry": "doodle"},
    {"name": "Pixel",       "entry": "pixel"},
    {"name": "Dream",       "entry": "dream"},
]
# Extra files for the System Folder (MacBinary). Glider 4.06 asks for its "MacsBug" file at every
# launch ("to improve performance"); it is part of Glider's distribution: a tiny 'dbgr' that the
# boot blocks load early so the alternate screen buffer can be reserved for page flipping.
# Checked in the browser emulator (2026-10-20): no nag, and the game draws and plays correctly.
EXTRA_SYSTEM = ["toolchain/build/demo/extras/MacsBug.bin"]
VISIBLE_DATA = {"The House", "The House 2", "Glider History", "Dream FLUX License"}
COLS, ROWS = [22, 98], [32, 94, 156, 218]       # desktop icon top-left corners (global coordinates): the apps
DOC_COL = 174                                   # the documents (Read Me, Credits & Licenses), a third column
WINDOW_GRID = ([14, 134, 254], [14, 76, 138])   # icon positions inside the disk window
DISK_WINDOW = (60, 40, 280, 420)                # top, left, bottom, right of the disk window

# File types that belong to the system, never to an app (as in toolchain/plusdisk/build_plus_hd.py).
SYSTEM_TYPES = {b"ZSYS", b"zsys", b"FNDR", b"cdev", b"INIT", b"RDEV", b"PRER", b"PRES", b"dahd",
                b"KCAP", b"CLIP", b"adev", b"ddev", b"fext", b"scri", b"help", b"scbk"}
FORBIDDEN_NAMES = [b"pokemon", b"poke", b"mario", b"plants", b"zombie", b"pvz", b"narrow"]
FORBIDDEN_CREATORS = {b"PkBl"}

# "Credits & Licenses" on the desktop (Devon, 2026-10-20: credits live on the Mac, not the web page).
CREDITS_NAME = "Credits & Licenses"
CREDITS = """CREDITS & LICENSES

THIS MAC'S OWN PROGRAMS
Endurance, Rail Runner, Blockworld, Doodle, Pixel and Dream, by Devon.

DREAM AND FLUX
Dream is for non-commercial use.

\u201cThis FLUX Model is licensed by Black Forest Labs Inc. under the FLUX Kommunity License. Copyright Black Forest Labs Inc. IN NO EVENT SHALL BLACK FOREST LABS INC. BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH USE OF THIS MODEL.\u201d

Dream is a modified work: a separate, much smaller image model (about one million ternary weights, 64x64 pixels, 16 grey levels) trained by the Dream project on black-and-white clip art generated with FLUX 3 Image Klein. It is not an official Black Forest Labs product and is not endorsed, approved or validated by Black Forest Labs. Any rights to use the FLUX Model are granted directly by Black Forest Labs under the license.

The full license is the file \u201cDream FLUX License\u201d in the Macintosh HD window.

THE 3D MAC (on the web page)
\u201cMacintosh 128K Computer (1984)\u201d (https://skfb.ly/6SLnE) by Daz is licensed under Creative Commons Attribution-NonCommercial (http://creativecommons.org/licenses/by-nc/4.0/).
Modified: converted for the web, live emulator screen, animated keys, textured case and badge added.

The floppy disk on the web page: this work is based on \u201cFloppy disk\u201d (https://sketchfab.com/3d-models/floppy-disk-15444a8efd38476f9457acce300edf0c) by drumdorf (https://sketchfab.com/drumdorf) licensed under CC-BY-SA-4.0 (http://creativecommons.org/licenses/by-sa/4.0/). Modified (resized, compressed textures) for this site; the modified model is also licensed CC-BY-SA-4.0.

GLIDER 4.06
\u00a9 1991\u20131992 John Calhoun, first published by Casady & Greene; freeware from its author. Included unchanged, with its MacsBug helper file.

THE TALKING MOOSE 1.21
Desk accessory by Steve Halls. He speaks through MacinTalk 1.31, \u00a9 Apple Computer, Inc.

THE EMULATOR
Mini vMac by Paul C. Pratt and contributors, GNU General Public License v2, in the WebAssembly build of Infinite Mac by Mihai Parparita (Apache License 2.0). Source of the exact code the web page runs:
https://github.com/mihaip/minivmac/tree/0bcab1d51ce240489b98b21b744ddfcddf01659e
https://github.com/mihaip/infinite-mac/tree/f988f537fff09c7b902e448a21cd8ac170e42c67
The license texts are on the web site, in the folder /licenses/.

THE WEB PAGE
three.js 0.186.1 (https://threejs.org), MIT License, with its meshopt decoder (meshoptimizer, MIT License); fflate 0.8.2 (unzipping downloads for Insert Disk), MIT License; the stuffit crate 0.4.0 by Ben Letchford (https://github.com/benletchford/stuffit-rs, StuffIt archives for Insert Disk), MIT License.
The badge on the 3D Mac is set in EB Garamond (\u00a9 The EB Garamond Project Authors), SIL Open Font License 1.1.
The machine's power, hum and floppy drive sounds are from Epidemic Sound, used under Devon's licence. The 3D keyboard's and mouse's click sounds are cut from two recordings on Freesound, both CC0 (public domain): \u201cTyping on Apple Standard Keyboard (ALPS SKCM Orange)\u201d by robni7 (https://freesound.org/s/680714/) and \u201cMouses (M0100, G5431, M2706)\u201d by SieuAmThanh (https://freesound.org/s/611812/).

APPLE
The Macintosh Plus ROM, System 6.0 and MacinTalk are copyright Apple Computer, Inc. This is an emulated period system; Apple has nothing to do with this site. Macintosh and Mac are trademarks of Apple Inc.
"""

# The Read Me: keybinds one per line, from the same lists as the web page (src/controls.js).
README_APPS = [
    ("Endurance", [("A / D", "Steer (or the mouse)"), ("W / Space", "Gas (or the mouse button)"), ("S", "Brake"),
                   ("Return", "Start a race"), ("P", "Pause"), ("M", "Sound on/off"), ("Esc or `", "Title, then quit")]),
    ("Rail Runner", [("A / D", "Change lanes"), ("W", "Jump"), ("S", "Roll"), ("Space", "Hoverboard"),
                     ("Return", "Start"), ("M", "Sound on/off"), ("Esc or `", "Title, then quit")]),
    ("Blockworld", [("W A S D", "Walk"), ("Space", "Jump / swim up"), ("Mouse", "Look (Lock mouse on the web page)"),
                    ("Click", "Mine / hit"), ("E", "Place / use / eat / bow"), ("Shift E", "Place on a workbench"),
                    ("1-9", "Hotbar slot"), ("Tab", "Inventory and crafting"), ("V", "View distance"),
                    ("Esc or `", "Menu"), ("Esc, then S", "View size"), ("Esc, then N", "New world (Y to confirm)"),
                    ("Esc, then R", "Back to the starting world (Y to confirm)"), ("Esc, then Q", "Save and quit")]),
    ("Glider 4.06", [("Command N", "New game (Ctrl N in the browser)"), (",", "Fly left"), (".", "Fly right"),
                     ("Return", "Battery boost"), ("Space", "Rubber band"), ("Tab", "Pause"),
                     ("Command Q", "Quit")]),
    ("Doodle", [("Drag", "Draw with the chosen tool"), ("Finish it", "Type what it is, then Return"),
                ("Click", "Stop the AI drawing"), ("Command Z", "Undo"), ("File menu", "Save, Open, Quit")]),
    ("Pixel", [("Type", "Your message"), ("Return", "Send"), ("Delete", "Correct a typo"), ("File menu", "New Chat, Quit")]),
    ("Dream", [("Type", "A subject, e.g. house"), ("Return", "Dream it"), ("Surprise me", "Random subject"),
               ("Click", "Stop after this step"), ("File menu", "Copy, Save, Quit")]),
    ("Talking Moose", [("Apple menu", "Talking Moose"), ("Close box", "Close its settings window")]),
]
README = ("MACINTOSH PLUS, IN YOUR BROWSER\n\n"
          "Double-click an icon on the desktop to start it. In the browser, Ctrl is the Mac's Command key. "
          "Nothing you change here is kept: reloading the web page starts this Mac afresh.\n\n"
          + "\n".join(f"{name}\n" + "\n".join(f"   {k}   -   {a}" for k, a in rows) + "\n" for name, rows in README_APPS)
          + "\nCredits and licenses: open \"Credits & Licenses\" on the desktop.\n\nWhen you are done: Special > Shut Down.\n")


class HFS:
    """hfsutils with a private $HOME (hfsutils keeps its current volume in $HOME/.hcwd)."""
    def __init__(self):
        self.home = tempfile.mkdtemp(prefix="hfs_home_")
        self.env = dict(os.environ, HOME=self.home)

    def __call__(self, *args, check=True):
        r = subprocess.run([a if isinstance(a, bytes) else str(a).encode("mac_roman") for a in args],
                           env=self.env, capture_output=True)
        if check and r.returncode:
            sys.exit(f"failed: {args}\n{r.stderr.decode(errors='replace')}")
        return r.stdout




def raw_floppy(path, dst):
    """Copy a floppy image to dst as a raw image (DiskCopy 4.2 header stripped); return its boot blocks."""
    d = open(path, "rb").read()
    if path.endswith(".dc42"):
        d = d[84:84 + struct.unpack(">I", d[0x40:0x44])[0]]
    open(dst, "wb").write(d)
    return d[:1024]


def hfs_partition(path):
    """(start block, count) of the Apple_HFS partition of an Apple-partitioned image, or None."""
    with open(path, "rb") as f:
        d = f.read(512 * 64)
    if d[:2] != b"ER" or d[512:514] != b"PM":
        return None
    n = struct.unpack(">I", d[516:520])[0]
    for i in range(min(n, 63)):
        e = d[512 * (1 + i):512 * (2 + i)]
        if e[48:80].split(b"\0")[0] == b"Apple_HFS":
            return struct.unpack(">II", e[8:16])
    return None


def ls(h, path):
    """[(kind, type, creator, name)] of a folder, names as raw Mac Roman bytes."""
    import re
    out = []
    for line in h(b"hls", b"-la", path).splitlines():
        m = re.match(rb"^([fd])(\S*)\s+(?:(.{4})/(.{4})\s+\d+\s+\d+|\d+ items?)\s+\w{3}\s+\d+\s+\S+\s(.*)$", line)
        if m:
            out.append((m.group(1), m.group(3), m.group(4), m.group(5)))
    return out


def macbinary_name(path):
    d = open(path, "rb").read(128)
    return d[2:2 + d[1]]


def find_app_folder(h, app):
    for f in (b":System Folder:", b":", b":System Folder:Startup Items:"):
        try:
            if any(n == app for k, t, c, n in ls(h, f) if k == b"f"):
                return f
        except SystemExit:
            pass
    def walk(path, depth):
        for k, t, c, n in ls(h, path):
            if k == b"f" and n == app:
                return path
            if k == b"d" and depth < 4:
                r = walk(path + n + b":", depth + 1)
                if r:
                    return r
    return walk(b":", 0)


def files_from_entry(key, work):
    """[(MacBinary path, Mac name)] for a games.json entry: the app first, then its data files."""
    g = json.load(open(GAMES_JSON))[key]
    if not g.get("disk"):
        sys.exit(f"{key}: no floppy in games.json")
    img = os.path.join(work, f"{key}.img")
    bb = raw_floppy(os.path.join(ROOT, g["disk"]), img)
    app = bb[0x5B:0x5B + bb[0x5A]]                       # bbHelloName: the startup application
    if bb[:2] != b"LK" or not app or app == b"Finder":
        sys.exit(f"{key}: its floppy does not start an app")
    out = []
    h = HFS()
    h(b"hmount", img)
    where = find_app_folder(h, app)
    if where is None:
        sys.exit(f"{key}: app {app!r} not on its floppy")
    for k, t, c, n in ls(h, where):
        if k == b"f" and (n == app or t not in SYSTEM_TYPES):
            dst = os.path.join(work, f"{key}_{len(out)}.bin")
            h(b"hcopy", b"-m", where + n, dst)
            out.append((dst, n))
    h(b"humount")
    out.sort(key=lambda x: x[1] != app)                  # the app first
    if g.get("flop2"):
        raw2 = os.path.join(work, f"{key}_flop2.img")
        raw_floppy(os.path.join(ROOT, g["flop2"]), raw2)
        h = HFS()
        h(b"hmount", raw2)
        def walk(path):
            for k, t, c, n in ls(h, path):
                if k == b"d":
                    walk(path + n + b":")
                elif t not in SYSTEM_TYPES:
                    dst = os.path.join(work, f"{key}_{len(out)}.bin")
                    h(b"hcopy", b"-m", path + n, dst)
                    out.append((dst, n))
        walk(b":")
        h(b"humount")
    return out

# ---------------------------------------------------------------- HFS catalog editing

class Volume:
    """Just enough HFS to find catalog records and rewrite Finder info in place."""
    def __init__(self, img):
        self.img = img
        mdb = img[1024:1024 + 162]
        if mdb[:2] != b"BD":
            sys.exit("not an HFS volume")
        self.alblksiz = struct.unpack(">I", mdb[0x14:0x18])[0]
        self.alblst = struct.unpack(">H", mdb[0x1C:0x1E])[0]
        self.ct_size = struct.unpack(">I", mdb[0x92:0x96])[0]
        self.ct_ext = [struct.unpack(">HH", mdb[0x96 + 4 * i:0x9A + 4 * i]) for i in range(3)]
        hdr = self.node(0)
        self.node_size = struct.unpack(">H", hdr[14 + 18:14 + 20])[0]
        assert self.node_size == 512
        self.first_leaf = struct.unpack(">I", hdr[14 + 10:14 + 14])[0]

    def node_offset(self, n):
        """Byte offset of catalog node n (512-byte nodes, catalog extents in the MDB)."""
        off = n * 512
        for start, count in self.ct_ext:
            span = count * self.alblksiz
            if off < span:
                return self.alblst * 512 + start * self.alblksiz + off
            off -= span
        raise ValueError("catalog node outside its extents")

    def node(self, n):
        o = self.node_offset(n)
        return self.img[o:o + 512]

    def records(self):
        """Yield (byte offset of record data, parentID, name, data) for every leaf record."""
        n = self.first_leaf
        while n:
            o = self.node_offset(n)
            nd = self.img[o:o + 512]
            flink = struct.unpack(">I", nd[0:4])[0]
            kind = struct.unpack(">b", nd[8:9])[0]
            nrecs = struct.unpack(">H", nd[10:12])[0]
            assert kind == -1, "expected a leaf node"
            for i in range(nrecs):
                ro = struct.unpack(">H", nd[510 - 2 * i:512 - 2 * i])[0]
                klen = nd[ro]
                parent = struct.unpack(">I", nd[ro + 2:ro + 6])[0]
                name = bytes(nd[ro + 7:ro + 7 + nd[ro + 6]])
                do = ro + 1 + klen
                if do & 1:
                    do += 1
                yield o + do, parent, name, nd[do]
            n = flink

    def find(self, parent, name):
        for o, p, n, kind in self.records():
            if p == parent and n.lower() == name.lower() and kind in (1, 2):
                return o, kind
        sys.exit(f"catalog record not found: {name!r}")

    def set_file_finfo(self, name, location, on_desk, invisible=False, inited=False):
        """Finder info of a root-level file: icon position (global coordinates for desktop items,
        window coordinates otherwise), isOnDesk (Finder flag bit 0: System 6 keeps desktop items in
        the root directory and marks them with it), invisible, and hasBeenInited (cleared, the
        Finder registers the file's bundle (its icons) in the Desktop file on the first boot)."""
        o, kind = self.find(2, name)
        assert kind == 2, f"{name!r} is not a file"
        fi = o + 4                                   # filUsrWds (FInfo)
        flags = struct.unpack(">H", self.img[fi + 8:fi + 10])[0]
        flags &= ~(0x4000 | 0x0100 | 0x0001)
        if invisible:
            flags |= 0x4000
        if inited:
            flags |= 0x0100
        if on_desk:
            flags |= 0x0001
        h, v = location
        struct.pack_into(">HhhH", self.img, fi + 8, flags, v, h, 0xFFFE if on_desk else 0)

    def finfo(self, name):
        o, kind = self.find(2, name)
        fi = o + 4
        return struct.unpack(">4s4sHhhh", self.img[fi:fi + 16])

    def set_folder_dinfo(self, name, location, parent=2):
        o, kind = self.find(parent, name)
        assert kind == 1, f"{name!r} is not a folder"
        di = o + 22                                  # dirUsrInfo (DInfo): frRect, frFlags, frLocation, frView
        h, v = location
        struct.pack_into(">hh", self.img, di + 10, v, h)

    def set_root_window(self, rect):
        """The disk window's rectangle (DInfo.frRect of the root folder, its record in parent 1)."""
        for o, p, n, kind in self.records():
            if p == 1 and kind == 1:
                struct.pack_into(">hhhh", self.img, o + 22, *rect)
                return
        sys.exit("root folder record not found")



# ---------------------------------------------------------------- build

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--size-mb", type=int, default=16)
    ap.add_argument("--out", default=os.path.join(HERE, "build", "disk", "hd_unbaked.dsk"))
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    work = tempfile.mkdtemp(prefix="webdisk_")

    # 1. System Folder and boot blocks from the demo disk's HFS partition (copied: hfsutils may
    #    touch images it mounts).
    sysimg = os.path.join(work, "system.hd")
    shutil.copyfile(SYSTEM_FROM, sysimg)
    part = hfs_partition(sysimg)
    with open(sysimg, "rb") as f:
        f.seek((part[0] if part else 0) * 512)
        bootblocks = f.read(1024)
    assert bootblocks[:2] == b"LK", "no boot blocks on the System source"
    print("boot blocks: system %r, finder %r, system heap %d KB" % (
        bootblocks[0x0B:0x0B + bootblocks[0x0A]].decode("mac_roman"),
        bootblocks[0x1B:0x1B + bootblocks[0x1A]].decode("mac_roman"),
        struct.unpack(">I", bootblocks[0x86:0x8A])[0] // 1024))
    h = HFS()
    h(b"hmount", sysimg, b"1" if part else b"0")
    sysfiles = []
    for k, t, c, n in ls(h, b":System Folder:"):
        if k == b"f":
            dst = os.path.join(work, f"sys_{len(sysfiles)}.bin")
            h(b"hcopy", b"-m", b":System Folder:" + n, dst)
            sysfiles.append((dst, n))
    teachtext = os.path.join(work, "teachtext.bin")
    h(b"hcopy", b"-m", b":TeachText", teachtext)
    h(b"humount")
    print("System Folder:", ", ".join(n.decode("mac_roman") for _, n in sysfiles))

    # 2. The apps and their data files.
    apps = []                                            # (desktop name, [(bin, mac name)])
    for spec in APPS:
        if "entry" in spec:
            files = files_from_entry(spec["entry"], work)
            src = f"games.json:{spec['entry']}"
        else:
            files = [(os.path.join(ROOT, p), macbinary_name(os.path.join(ROOT, p))) for p in spec["macbinary"]]
            src = "MacBinary"
        if spec.get("icon"):
            app_bin = files[0][0]
            if not app_bin.startswith(work):                 # never touch a project's own file
                copy = os.path.join(work, f"icon_{len(apps)}.bin")
                shutil.copyfile(app_bin, copy)
                files[0] = (copy, files[0][1])
            subprocess.run([sys.executable, "-I", os.path.join(ROOT, "toolchain", "icons", "add_icon.py"),
                            files[0][0], "--preset", spec["icon"], "--out", files[0][0]], check=True,
                           capture_output=True)
        apps.append((spec["name"].encode("mac_roman"), files))
        print(f"  {spec['name']:<12} from {src}: " + ", ".join(n.decode("mac_roman") for _, n in files))

    # 3. A fresh raw HFS volume (no partition map: Mini vMac mounts the volume directly).
    with open(out, "wb") as f:
        f.truncate(a.size_mb * 1024 * 1024)
    h = HFS()
    h(b"hformat", b"-l", b"Macintosh HD", out)
    h(b"hmkdir", b":System Folder")
    for src, name in sysfiles:
        h(b"hcopy", b"-m", src, b":System Folder:" + name)
    for p in EXTRA_SYSTEM:
        h(b"hcopy", b"-m", os.path.join(ROOT, p), b":System Folder:" + macbinary_name(os.path.join(ROOT, p)))
    h(b"hattrib", b"-b", b":System Folder")              # bless it
    h(b"hcopy", b"-m", teachtext, b":TeachText")
    placed = []                                          # (name, where)
    for name, files in apps:
        for i, (src, fname) in enumerate(files):
            dest = name if i == 0 else fname
            h(b"hcopy", b"-m", src, b":" + dest)
            where = "desk" if i == 0 else ("window" if fname.decode("mac_roman") in VISIBLE_DATA else "hidden")
            placed.append((dest, where))
    txt = os.path.join(work, "readme.txt")
    with open(txt, "wb") as f:                            # Mac Roman, CR line ends, copied raw
        f.write(README.replace("\n", "\r").encode("mac_roman"))
    h(b"hcopy", b"-r", txt, b":Read Me")
    h(b"hattrib", b"-t", b"TEXT", b"-c", b"ttxt", b":Read Me")
    placed.append((b"Read Me", "doc"))
    cred = os.path.join(work, "credits.txt")
    data = CREDITS.replace("\n", "\r").encode("mac_roman")
    assert len(data) < 32000, "TeachText opens files up to 32 KB"
    with open(cred, "wb") as f:
        f.write(data)
    h(b"hcopy", b"-r", cred, b":" + CREDITS_NAME.encode("mac_roman"))
    h(b"hattrib", b"-t", b"TEXT", b"-c", b"ttxt", b":" + CREDITS_NAME.encode("mac_roman"))
    placed.append((CREDITS_NAME.encode("mac_roman"), "doc"))
    listing = h(b"hls", b"-la", b":")
    vol = h(b"hvol").decode()
    h(b"humount")
    print(listing.decode("mac_roman"))
    print([l.strip() for l in vol.splitlines() if "free" in l][0])

    # 4. Boot blocks, then Finder info straight in the catalog.
    img = bytearray(open(out, "rb").read())
    img[0:1024] = bootblocks
    v = Volume(img)
    desk = [(c, r) for c in COLS for r in ROWS]
    docs = [(DOC_COL, r) for r in ROWS]
    win = [(c, r) for r in WINDOW_GRID[1] for c in WINDOW_GRID[0]]
    v.set_folder_dinfo(b"System Folder", win.pop(0))
    v.set_file_finfo(b"TeachText", win.pop(0), False)
    hidden_at = 300
    for name, where in placed:
        if where == "desk":
            v.set_file_finfo(name, desk.pop(0), True)
        elif where == "doc":
            v.set_file_finfo(name, docs.pop(0), True)
        elif where == "window":
            v.set_file_finfo(name, win.pop(0), False)
        else:
            v.set_file_finfo(name, (hidden_at, 300), False, invisible=True)
            hidden_at += 40
    v.set_root_window(DISK_WINDOW)

    # 5. Guard: nothing excluded may be on the volume.
    for o, p, n, kind in v.records():
        low = n.lower()
        if any(bad in low for bad in FORBIDDEN_NAMES):
            sys.exit(f"forbidden file on the disk: {n!r}")
        if kind == 2 and bytes(img[o + 8:o + 12]) in FORBIDDEN_CREATORS:
            sys.exit(f"forbidden creator on the disk: {n!r}")
    open(out, "wb").write(img)
    shutil.rmtree(work, ignore_errors=True)
    print(f"{out}  ({a.size_mb} MB raw HFS volume, unbaked)")


if __name__ == "__main__":
    main()

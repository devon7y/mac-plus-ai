#!/usr/bin/env python3
"""Build Narrow Screen: assemble the INIT and cdev, pack them into a Mac resource
fork, wrap it as MacBinary, and install it on a copy of a System 6 boot disk.

Usage: build.py [--left PX] [--right PX] [--disk SRC.dc42] [--out OUT.dc42]
"""
import argparse
import os
import struct
import subprocess
import sys
import time

# hfsutils keeps its "current volume" in $HOME/.hcwd, shared by every process. Give this
# script a private HOME so parallel disk builds can't write into each other's images.
import tempfile as _tempfile
os.environ["HOME"] = _tempfile.mkdtemp(prefix="hfs-home-")


HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "src")
BUILD = os.path.join(HERE, "build")
EMU = os.path.join(os.path.dirname(HERE), "emu")

FILE_NAME = "Narrow Screen"
FILE_TYPE, CREATOR = b"cdev", b"NScr"
RID = -4064                       # cdev-owned resource ID
FLAG_HAS_BUNDLE = 0x2000
FLAG_INITED = 0x0100            # Finder has already placed the icon


def run(*cmd):
    subprocess.run(cmd, check=True)


def assemble(name, debug=False, syms=()):
    obj, binf = os.path.join(BUILD, name + ".o"), os.path.join(BUILD, name + ".bin")
    defs = ["--defsym", "DEBUG=1"] if debug else []
    for sym in syms:
        defs += ["--defsym", sym if "=" in sym else sym + "=1"]
    run("m68k-elf-as", "-m68000", *defs, "-I", SRC, "-o", obj, os.path.join(SRC, name + ".s"))
    relocs = subprocess.run(["m68k-elf-objdump", "-r", obj], capture_output=True, text=True).stdout
    if "R_68K" in relocs:
        sys.exit(f"{name}: code is not position-independent:\n{relocs}")
    run("m68k-elf-objcopy", "-O", "binary", "-j", ".text", obj, binf)
    return open(binf, "rb").read()


def pstr(s):
    b = s.encode("mac_roman")
    return bytes([len(b)]) + b


def rect(t, l, b, r):
    return struct.pack(">4h", t, l, b, r)


# ---- Dialog items for the Control Panel area (window-local coordinates) ----
DISABLED, BTN, RESCTRL, STATTEXT = 128, 4, 7, 8
LEFT_BAR, RIGHT_BAR = (72, 148, 88, 268), (100, 148, 116, 268)
ITEMS = [
    (STATTEXT | DISABLED, (10, 100, 26, 315), pstr("Narrow Screen")[1:]),
    (STATTEXT | DISABLED, (30, 100, 62, 315), pstr("Margins hidden behind the bezel, in pixels:")[1:]),
    (STATTEXT | DISABLED, (72, 100, 88, 145), b"Left"),
    (RESCTRL, LEFT_BAR, struct.pack(">h", RID)),
    (STATTEXT | DISABLED, (72, 276, 88, 315), b"0"),
    (STATTEXT | DISABLED, (100, 100, 116, 145), b"Right"),
    (RESCTRL, RIGHT_BAR, struct.pack(">h", RID + 1)),
    (STATTEXT | DISABLED, (100, 276, 116, 315), b"0"),
    (STATTEXT | DISABLED, (128, 100, 208, 315),
     "The lines on screen mark the new edges. Left moves in 16-pixel "
     "steps. Restart to apply. Hold Shift while starting up to skip.".encode("mac_roman")),
    (BTN, (220, 225, 240, 305), b"Restart"),
]


def ditl():
    out = struct.pack(">h", len(ITEMS) - 1)
    for kind, r, data in ITEMS:
        out += struct.pack(">I", 0) + rect(*r) + bytes([kind, len(data)]) + data
        if len(data) % 2:
            out += b"\0"
    return out


def cntl(r, value, vmax):
    # rect, value, visible, fill, max, min, procID (16 = scroll bar), refCon, title
    return rect(*r) + struct.pack(">hBBhhhI", value, 1, 0, vmax, 0, 16, 0) + pstr("")


def icon():
    """32x32 compact-Mac icon with inward arrows, plus its mask (ICN# data)."""
    px = [[0] * 32 for _ in range(32)]
    mask = [[0] * 32 for _ in range(32)]

    def box(t, l, b, r, fill=False, grid=px):
        for y in range(t, b + 1):
            for x in range(l, r + 1):
                if fill or y in (t, b) or x in (l, r):
                    grid[y][x] = 1
    box(1, 5, 26, 26, fill=True, grid=mask)        # case
    box(27, 7, 30, 24, fill=True, grid=mask)       # foot
    box(1, 5, 26, 26)
    box(27, 7, 30, 24)
    box(4, 8, 18, 23)                              # screen
    for i in range(5):                             # arrows pointing inward
        for y in range(11 - i, 12 + i):
            px[y][10 + i] = 1
            px[y][21 - i] = 1
    for x in range(17, 23):                        # floppy slot
        px[22][x] = 1

    def pack(g):
        return b"".join(struct.pack(">I", int("".join(map(str, row)), 2)) for row in g)
    return pack(px) + pack(mask)


def resource_fork(res):
    """res: list of (type, id, attrs, data). Returns a resource fork."""
    by_type = {}
    for t, rid, attrs, data in res:
        by_type.setdefault(t, []).append((rid, attrs, data))
    data_area, refs = b"", {}
    for t in by_type:
        for rid, attrs, data in by_type[t]:
            refs.setdefault(t, []).append((rid, attrs, len(data_area)))
            data_area += struct.pack(">I", len(data)) + data
    types = list(by_type)
    type_list = struct.pack(">h", len(types) - 1)
    ref_lists = b""
    ref_base = 2 + 8 * len(types)
    for t in types:
        type_list += t + struct.pack(">hH", len(refs[t]) - 1, ref_base + len(ref_lists))
        for rid, attrs, off in refs[t]:
            ref_lists += struct.pack(">hhI", rid, -1, (attrs << 24) | off) + b"\0\0\0\0"
    data_off = 256
    map_body = type_list + ref_lists
    map_len = 28 + len(map_body)
    map_off = data_off + len(data_area)
    header = struct.pack(">IIII", data_off, map_off, len(data_area), map_len)
    res_map = header + b"\0" * 6 + struct.pack(">hHH", 0, 28, map_len) + map_body
    return header + b"\0" * (256 - 16) + data_area + res_map


def crc16_xmodem(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def macbinary(name, ftype, creator, flags, rsrc):
    h = bytearray(128)
    nb = name.encode("mac_roman")
    h[1] = len(nb)
    h[2:2 + len(nb)] = nb
    h[65:69], h[69:73] = ftype, creator
    h[73] = flags >> 8
    h[83:87] = struct.pack(">I", 0)
    h[87:91] = struct.pack(">I", len(rsrc))
    now = int(time.time()) + 2082844800           # seconds since 1904 (Mac epoch)
    h[91:99] = struct.pack(">II", now, now)       # creation, modification dates
    h[101] = flags & 0xFF
    h[122] = h[123] = 129                          # MacBinary II
    h[124:126] = struct.pack(">H", crc16_xmodem(bytes(h[:124])))
    pad = (-len(rsrc)) % 128
    return bytes(h) + rsrc + b"\0" * pad


def dc42_checksum(data):
    s = 0
    for i in range(0, len(data), 2):
        s = (s + ((data[i] << 8) | data[i + 1])) & 0xFFFFFFFF
        s = ((s >> 1) | (s << 31)) & 0xFFFFFFFF
    return s


def write_dc42(template, raw, out):
    """Write a Disk Copy 4.2 image, keeping the template's 12-byte sector tags.
    (MAME corrupts sectors when writing back to an 800K image that has no tags.)"""
    t = open(template, "rb").read()
    hdr = bytearray(t[:84])
    dsize, tsize = struct.unpack(">II", t[0x40:0x48])
    tags = t[84 + dsize:84 + dsize + tsize]
    if len(tags) != 12 * (len(raw) // 512):
        tags = b"\0" * (12 * (len(raw) // 512))
    hdr[0x40:0x44] = struct.pack(">I", len(raw))
    hdr[0x44:0x48] = struct.pack(">I", len(tags))
    hdr[0x48:0x4C] = struct.pack(">I", dc42_checksum(raw))
    hdr[0x4C:0x50] = struct.pack(">I", dc42_checksum(tags[12:]))
    open(out, "wb").write(bytes(hdr) + raw + tags)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--left", type=int, default=32, help="default left margin (multiple of 16)")
    ap.add_argument("--right", type=int, default=32, help="default right margin")
    # A copy of Apple's disk booted once in MAME, so System 6 has already set its
    # first-boot catalog flags (MAME can corrupt those writes; see README).
    ap.add_argument("--disk", default=os.path.join(EMU, "disks/system-6.0-booted/6.0 System Tools.dc42"))
    ap.add_argument("--out", default=os.path.join(EMU, "work/narrow_test.dc42"))
    ap.add_argument("--debug", action="store_true", help="trace cdev calls into the screen margin")
    ap.add_argument("--init-sym", action="append", default=[], help="assembler symbol for init.s (experiments)")
    a = ap.parse_args()
    if a.left % 16 or not 0 <= a.left <= 128 or not 0 <= a.right <= 128:
        sys.exit("left must be a multiple of 16 in 0..128; right in 0..128")
    os.makedirs(BUILD, exist_ok=True)

    init_code, cdev_code = assemble("init", syms=a.init_sym), assemble("cdev", a.debug)
    res = [
        (b"INIT", 0, 0x50, init_code),                    # sysHeap + locked
        (b"cdev", RID, 0x10, cdev_code),                  # locked: Control Panel may cache its address
        (b"nsSt", RID, 0x00, struct.pack(">4h", 1, a.left, a.right, 0)),
        (b"DITL", RID, 0x20, ditl()),
        (b"CNTL", RID, 0x20, cntl(LEFT_BAR, a.left // 16, 8)),
        (b"CNTL", RID + 1, 0x20, cntl(RIGHT_BAR, a.right, 128)),
        (b"nrct", RID, 0x20, struct.pack(">h", 1) + rect(-1, 87, 255, 322)),
        (b"mach", RID, 0x00, struct.pack(">HH", 0xFFFF, 0x0000)),
        (b"ICN#", RID, 0x20, icon()),
        (b"FREF", RID, 0x00, FILE_TYPE + struct.pack(">h", 0) + pstr("")),
        (b"BNDL", RID, 0x00, CREATOR + struct.pack(">hh", 0, 1)
            + b"ICN#" + struct.pack(">hhh", 0, 0, RID)
            + b"FREF" + struct.pack(">hhh", 0, 0, RID)),
        (CREATOR, 0, 0x00, pstr("Narrow Screen 1.0")),
        (b"STR ", RID, 0x00, pstr("Narrow Screen")),
    ]
    rsrc = resource_fork(res)
    open(os.path.join(BUILD, "Narrow Screen.rsrc"), "wb").write(rsrc)
    mb_path = os.path.join(BUILD, "Narrow Screen.bin")
    open(mb_path, "wb").write(macbinary(FILE_NAME, FILE_TYPE, CREATOR, FLAG_HAS_BUNDLE | FLAG_INITED, rsrc))
    print(f"INIT {len(init_code)} bytes, cdev {len(cdev_code)} bytes, resource fork {len(rsrc)} bytes")

    raw_path = os.path.join(BUILD, "boot.img")
    disk = open(a.disk, "rb").read()
    open(raw_path, "wb").write(disk[84:84 + struct.unpack(">I", disk[0x40:0x44])[0]])
    run("hmount", raw_path)
    try:
        subprocess.run(["hdel", f":System Folder:{FILE_NAME}"], capture_output=True)
        run("hcopy", "-m", mb_path, ":System Folder:")
        subprocess.run(["hls", "-l", ":System Folder:" + FILE_NAME])
    finally:
        subprocess.run(["humount"], capture_output=True)
    write_dc42(a.disk, open(raw_path, "rb").read(), a.out)
    print(f"Disk image: {a.out}  (defaults: left {a.left}, right {a.right})")


if __name__ == "__main__":
    main()

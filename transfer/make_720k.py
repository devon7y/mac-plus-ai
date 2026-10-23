#!/usr/bin/env python3
"""Build a 720K (DD, MFM) Mac HFS floppy image holding Narrow Screen and a Read Me.

hfsutils won't format volumes under 800K, so this uses machfs.
Usage: make_720k.py <machfs-path> [out.img]
"""
import os
import struct
import sys
import time

sys.path.insert(0, sys.argv[1])
import machfs

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "NarrowScreen-720K-HFS.img")

mb = open(os.path.join(ROOT, "narrowscreen/build/Narrow Screen.bin"), "rb").read()
dlen, rlen = struct.unpack(">II", mb[83:91])
rsrc = mb[128 + ((dlen + 127) // 128) * 128:][:rlen]
now = int(time.time()) + 2082844800                 # Mac epoch (1904)

v = machfs.Volume()
v.name = "Narrow Screen"
ns = machfs.File()
ns.type, ns.creator, ns.flags = b"cdev", b"NScr", 0x2000   # has bundle
ns.rsrc, ns.data = rsrc, b""
ns.crdate = ns.mddate = now
v["Narrow Screen"] = ns
rm = machfs.File()
rm.type, rm.creator = b"TEXT", b"ttxt"
rm.data = open(os.path.join(HERE, "readme.txt"), "rb").read().replace(b"\n", b"\r")
rm.crdate = rm.mddate = now
v["Read Me"] = rm

img = v.write(size=737280, bootable=False)
assert len(img) == 737280, len(img)
open(out, "wb").write(img)
print(f"wrote {out} ({len(img)} bytes)")

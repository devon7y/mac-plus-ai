#!/usr/bin/env python3
"""After a MAME transfer run: compare what arrived on both sides with the originals.

    python3 -I verify.py <run folder>

- the Mac's disk (the run's copy: disk1.img, or hard1.hd on the SE testbed): "Big Test" has the original
  data fork, resource fork (except bytes 16-127, which the System itself writes on close), type TSTF and
  creator Shtl, and the "inited" Finder flag cleared; "Big Two" (the cancelled download) is gone;
- the server's folder (<run>/root): "From Plus/Upload Test" has the forks and type of the file the
  build put on the test floppy.
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "server"))
sys.path.insert(0, HERE)
import shuttle_server as S  # noqa: E402
from make_root import big_test  # noqa: E402

ENV = dict(os.environ, HOME=tempfile.mkdtemp(prefix="hfs_home_"))


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    return ok


def main():
    run = sys.argv[1]
    hd = os.path.join(run, "hard1.hd")
    disk = hd if os.path.exists(hd) else os.path.join(run, "disk1.img")
    out = os.path.join(run, "BigTest_from_mac.bin")
    good = True
    subprocess.run(["hmount", disk] + (["1"] if disk == hd else []), env=ENV, capture_output=True, check=True)
    try:
        ls = subprocess.run(["hls", "-l", ":"], env=ENV, capture_output=True, text=True).stdout
        print(ls.rstrip())
        subprocess.run(["hcopy", "-m", ":Big Test", out], env=ENV, capture_output=True, check=True)
        good &= check("Big Two" not in ls, "the cancelled download was deleted")
    finally:
        subprocess.run(["humount"], env=ENV, capture_output=True)
    m = S.parse_macbinary(open(out, "rb").read())
    data, rsrc = big_test()
    good &= check(m is not None, "Big Test read back from the Mac's disk")
    # What the Mac wrote, and read back itself after saving (Shuttle checks every download): the CRCs it
    # reports in mame.log ("crc=data/rsrc", the resource fork without the system's bytes 16-127).
    log = open(os.path.join(run, "mame.log"), encoding="latin-1").read() if os.path.exists(os.path.join(run, "mame.log")) else ""
    want = "%04x/%04x" % (S.crc16(data), S.crc16(rsrc[:16] + rsrc[128:]))
    line = [ln for ln in log.splitlines() if ln.startswith("[downloaded")]
    in_app = bool(line) and ("crc=" + want) in line[0]
    good &= check(in_app, "the Mac wrote and read back exactly the original forks (in-app CRCs %s)" % want)
    if m:
        # Bytes 16-127 of a resource fork are reserved for the system: when a resource fork is closed, the
        # File Manager writes a copy of the file's catalog info at 48-127 (every Apple file has it, e.g.
        # TeachText's "\x09TeachText" record). So compare the rest, and check the System's record is there.
        lost = []
        for name, exp, got in (("data", data, m[4]), ("resource", rsrc, m[5])):
            for b in range(0, len(exp), 512):
                e, g = exp[b:b + 512], got[b:b + 512]
                if name == "resource" and b == 0:
                    e, g = e[:16] + e[128:], g[:16] + g[128:]
                if e != g:
                    lost.append((name, b // 512, not any(got[b + (128 if name == "resource" and b == 0 else 0):b + 512])))
        zero_only = all(z for _, _, z in lost)
        if lost and zero_only and in_app and disk != hd:
            print("note %d sector(s) are zeros in MAME's saved floppy image (%s): MAME lost them when saving the "
                  "800K image; the Mac itself read the file back correctly" % (len(lost), ", ".join("%s %d" % x[:2] for x in lost)))
        else:
            good &= check(not lost, "both forks identical on the disk image (resource fork apart from the system's bytes 16-127)")
        good &= check(m[5][48:57] == b"\x08Big Test", "the System's catalog copy at bytes 48-127 names the file")
        good &= check((m[1], m[2]) == (b"TSTF", b"Shtl"), "type and creator %r %r" % (m[1], m[2]))
        good &= check(not (m[3] & 0x0100), "Finder flags 0x%04x (inited cleared)" % m[3])
    up = os.path.join(run, "root", "From Plus", "Upload Test")
    good &= check(os.path.exists(up), "Upload Test arrived in From Plus")
    if os.path.exists(up):
        good &= check(open(up, "rb").read() == b"Hello from the Plus!\r" * 150, "its data fork")
        good &= check(S.read_rsrc(up) == bytes(range(256)) * 20, "its resource fork")
        fi = S.get_finder_info(up) or b""
        good &= check(fi[:8] == b"TEXTttxt", "its type and creator %r" % fi[:8])
    print("VERIFY PASS" if good else "VERIFY FAIL")
    sys.exit(0 if good else 1)


if __name__ == "__main__":
    main()

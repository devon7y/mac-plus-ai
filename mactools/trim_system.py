#!/usr/bin/env python3
"""Trim a System 6 System file down to what a single game needs on a Mac Plus.

Usage: trim_system.py <System.bin (MacBinary)> <out.bin> [--level safe|aggressive] [--keep-se]

safe:        drop desk accessories, Mac II/SE ROM patches, extra fonts and sizes,
             alert sounds (except Simple Beep), Color Picker, color icons, Mac II video driver.
aggressive:  also drop every font (the Plus ROM has Chicago 12, Geneva 9/12, Monaco 9),
             the Sound Manager synthesizers and the AppleTalk driver.
--keep-se:   keep the SE's ROM patch (PTCH 630, 15,712 bytes). Without it the disk does not
             boot a Macintosh SE (the lab's SE or MACHINE=macsefd); the Plus doesn't need it.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rsrc  # noqa: E402

KEEP_FONTS = {0, 12, 384, 393, 396, 512, 521}       # Chicago 12, Geneva 9/12, Monaco 9 (+ family names)
KEEP_FONDS = {0, 3, 4}


def drop(r, level, keep_se=False):
    t, i = r["type"], r["id"]
    name = r["name"] or b""
    if t == b"DRVR" and (name.startswith(b"\0") or name == b".Display_Video_Apple_TFB"):
        return True                                   # desk accessories, Mac II video card
    if t == b"PTCH" and i in (376, 630):              # ROM patches for Mac II ($178) and SE ($276)
        return not (keep_se and i == 630)
    if t == b"snd " and i != 1:
        return True
    if t == b"PACK" and i == 12:                      # Color Picker
        return True
    if t == b"cicn":
        return True
    if level == "safe":
        if t == b"FONT" and i not in KEEP_FONTS:
            return True
        if t == b"FOND" and i not in KEEP_FONDS:
            return True
    else:
        if t in (b"FONT", b"FOND", b"snth"):
            return True
        if t == b"DRVR" and name == b".XPP":
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--level", choices=("safe", "aggressive"), default="safe")
    ap.add_argument("--keep-se", action="store_true", help="keep PTCH 630 so the disk also boots an SE")
    a = ap.parse_args()
    header, data, fork = rsrc.read_macbinary(a.src)
    res = rsrc.parse(fork)
    kept = [r for r in res if not drop(r, a.level, a.keep_se)]
    out = rsrc.build(kept)
    rsrc.write_macbinary(a.out, header, data, out)
    print(f"{a.level}{' +SE' if a.keep_se else ''}: {len(res)} -> {len(kept)} resources, {len(fork)} -> {len(out)} bytes")


if __name__ == "__main__":
    main()

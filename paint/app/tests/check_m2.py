#!/usr/bin/env python3
"""After test_paint.lua: make the PNG fixture checks and the save/open round trips.

  check_m2.py fixture <out_dir>              -> fixture.png + Test7.bin (PNTG MacBinary) to put on the disk
  check_m2.py verify <out_dir> <disk image>  -> extracts Test1/Dlg/Test8 from the disk and compares
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageChops, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
MP = os.path.join(HERE, "..", "..", "tools", "macpaint.py")
sys.path.insert(0, os.path.dirname(MP))
import macpaint  # noqa: E402


def fixture(out):
    img = Image.new("1", (576, 720), 1)
    d = ImageDraw.Draw(img)
    d.rectangle((10, 10, 320, 260), outline=0, width=3)
    d.ellipse((60, 40, 200, 160), fill=0)
    d.line((0, 719, 575, 0), fill=0, width=2)
    d.text((220, 200), "PNG -> MacPaint", fill=0)
    for i in range(0, 576, 16):
        d.rectangle((i, 700, i + 7, 707), fill=0)
    img.save(os.path.join(out, "fixture.png"))
    macpaint.write_macbinary(os.path.join(out, "Test7.bin"), b"Test7",
                             macpaint.pntg_bytes(macpaint.png_to_page(os.path.join(out, "fixture.png"))))


def same(a, b):
    return ImageChops.difference(Image.open(a).convert("1"), Image.open(b).convert("1")).getbbox() is None


def verify(out, disk):
    env = dict(os.environ, HOME=tempfile.mkdtemp())
    run = lambda *c: subprocess.run(c, env=env, check=True, capture_output=True)
    run("hmount", disk)
    try:
        for src, dst in ((":System Folder:Test1", "Test1.bin"), (":Dlg", "Dlg.bin"), (":System Folder:Test8", "Test8.bin")):
            run("hcopy", "-m", src, os.path.join(out, dst))
    finally:
        subprocess.run(["humount"], env=env, capture_output=True)
    p = lambda n: os.path.join(out, n)
    for b in ("Test1", "Dlg", "Test8"):
        macpaint.page_to_png(macpaint.read_pntg(p(b + ".bin")), p(b + ".png"))
    for d in ("m2_saved", "m2_png_in", "m2_all_tools"):
        macpaint.page_to_png(open(p(d + ".bin"), "rb").read(), p(d + ".png"))
    ok = True
    for a, b, what in (("Test1.png", "m2_saved.png", "Mac save (mailbox) == page in memory"),
                       ("Dlg.png", "m2_saved.png", "Mac save (Save dialog) == page in memory"),
                       ("m2_png_in.png", "fixture.png", "PNG -> MacPaint -> opened on the Mac == PNG"),
                       ("Test8.png", "fixture.png", "PNG -> Mac open -> Mac save -> PNG == PNG")):
        s = same(p(a), p(b))
        ok &= s
        print(f"{'OK ' if s else 'BAD'} {what}")
    print("M2 FILES PASS" if ok else "M2 FILES FAIL")
    return ok


if __name__ == "__main__":
    if sys.argv[1] == "fixture":
        fixture(sys.argv[2])
    else:
        sys.exit(0 if verify(sys.argv[2], sys.argv[3]) else 1)

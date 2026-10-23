#!/usr/bin/env python3
"""Convert MacPaint documents (PNTG) to PNG and back, on the MacBook.

  macpaint.py topng   <in.pntg | in.bin (MacBinary)> <out.png>
  macpaint.py frompng <in.png> <out.bin> [--raw]    MacBinary (type PNTG, creator Ddle) for
                                                    hcopy -m, or a raw data fork with --raw
  macpaint.py dumptopng <page.bin> <out.png>        raw 576x720 page (72 bytes per row)

Format: 512-byte header (version, 38 patterns, padding), then 720 rows of 72 bytes, each
row PackBits-compressed separately. Black = 1. PNG input is thresholded at 50% gray and
cropped or padded (white) to 576x720 from the top-left.
"""
import os
import struct
import sys

from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "mactools"))
import rsrc  # noqa: E402  (MacBinary II writer with CRC)

W, H, RB = 576, 720, 72


def unpackbits(data, pos, n):
    out = bytearray()
    while len(out) < n:
        c = data[pos]
        pos += 1
        if c < 128:
            out += data[pos:pos + c + 1]
            pos += c + 1
        elif c > 128:
            out += bytes([data[pos]]) * (257 - c)
            pos += 1
    return bytes(out[:n]), pos


def packbits(row):
    out, i, n = bytearray(), 0, len(row)
    while i < n:
        j = i
        while j + 1 < n and row[j + 1] == row[i] and j - i < 127:
            j += 1
        if j > i:                                   # run of j - i + 1 equal bytes
            out += bytes([257 - (j - i + 1), row[i]])
            i = j + 1
            continue
        j = i                                       # literal: up to the next run of 2+
        while j < n and j - i < 128 and not (j + 1 < n and row[j + 1] == row[j]):
            j += 1
        if j == i:
            j = i + 1
        out += bytes([j - i - 1]) + row[i:j]
        i = j
    return bytes(out)


def read_pntg(path):
    d = open(path, "rb").read()
    if len(d) >= 128 and d[0] == 0 and d[65:69] == b"PNTG":       # MacBinary
        dl = struct.unpack(">I", d[83:87])[0]
        d = d[128:128 + dl]
    pos, rows = 512, []
    for _ in range(H):
        if pos >= len(d):
            rows.append(bytes(RB))
            continue
        r, pos = unpackbits(d, pos, RB)
        rows.append(r)
    return b"".join(rows)


def page_to_png(page, out):
    img = Image.frombytes("1", (W, H), bytes(b ^ 0xFF for b in page))
    img.save(out)


def png_to_page(path):
    img = Image.open(path).convert("L")
    page = bytearray(RB * H)
    w, h = min(img.width, W), min(img.height, H)
    px = img.load()
    for y in range(h):
        for x in range(w):
            if px[x, y] < 128:
                page[y * RB + x // 8] |= 0x80 >> (x % 8)
    return bytes(page)


def pntg_bytes(page):
    hdr = bytearray(512)                               # version 0: MacPaint's default patterns
    return bytes(hdr) + b"".join(packbits(page[y * RB:(y + 1) * RB]) for y in range(H))


def write_macbinary(path, name, data, ftype=b"PNTG", creator=b"Ddle"):
    hdr = bytearray(128)
    hdr[1] = len(name)
    hdr[2:2 + len(name)] = name
    hdr[65:69], hdr[69:73] = ftype, creator
    rsrc.write_macbinary(path, bytes(hdr), data, b"")


def main():
    cmd = sys.argv[1]
    if cmd == "topng":
        page_to_png(read_pntg(sys.argv[2]), sys.argv[3])
    elif cmd == "dumptopng":
        page_to_png(open(sys.argv[2], "rb").read()[:RB * H], sys.argv[3])
    elif cmd == "frompng":
        data = pntg_bytes(png_to_page(sys.argv[2]))
        out = sys.argv[3]
        if "--raw" in sys.argv:
            open(out, "wb").write(data)
        else:
            name = out.rsplit("/", 1)[-1].rsplit(".", 1)[0].encode("mac_roman")[:31]
            write_macbinary(out, name, data)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()

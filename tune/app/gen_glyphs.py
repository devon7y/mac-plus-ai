#!/usr/bin/env python3
"""Draw Tune's music glyphs (treble clef, sharp, flat, natural, rests) as 1-bit bitmaps and write
glyphs.c. Original drawings: curves and strokes drawn here with PIL at 4x and reduced; no font is
used. Two sizes: staff line spacing 6 px (big) and 4 px (small).

  python3 -I tune/app/gen_glyphs.py [--preview out.png]
"""
import math
import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
S = 8          # supersampling


def canvas(w, h):
    im = Image.new("L", (w * S, h * S), 0)
    return im, ImageDraw.Draw(im)


def pts(seq):
    return [(x * S, y * S) for x, y in seq]


def reduce(im, w, h, thr=96):
    small = im.resize((w, h), Image.BOX)
    return [[1 if small.getpixel((x, y)) >= thr else 0 for x in range(w)] for y in range(h)]


def clef(sp):
    """Treble clef for line spacing sp; the G line (2nd from bottom) is at y = top + 2.5 sp + 4 sp."""
    u = sp / 6.0
    w, h = int(round(14 * u)), int(round(42 * u))
    im, d = canvas(w, h)
    lw = max(1, int(round(1.6 * u * S)))
    gy = 27.5 * u                 # G line
    cx = 7.0 * u
    # spiral around the G line: from the centre outwards, counter-clockwise, then up as the main stroke
    sp_pts = []
    for i in range(0, 300):
        t = i / 300.0
        ang = math.pi * 0.5 + t * 2.0 * math.pi * 1.25
        r = (1.0 + 5.2 * t) * u
        sp_pts.append((cx + r * math.cos(ang) * 1.05, gy - 0.6 * u + r * math.sin(ang) * 1.1))
    end = sp_pts[-1]
    # main stroke from the spiral's end up and right to the top loop
    up = [end, (2.5 * u, 19 * u), (5.0 * u, 13.5 * u), (8.5 * u, 8 * u), (10 * u, 4 * u), (9.0 * u, 1.2 * u),
          (7.2 * u, 1.0 * u), (6.0 * u, 4 * u), (6.0 * u, 9 * u)]
    # stem down through the spiral to the tail
    stem = [(6.0 * u, 9 * u), (6.6 * u, 20 * u), (7.4 * u, 31 * u), (7.6 * u, 37 * u), (6.6 * u, 40 * u),
            (4.5 * u, 40.6 * u), (3.0 * u, 39.4 * u)]
    for seq in (sp_pts, up, stem):
        d.line(pts(seq), fill=255, width=lw, joint="curve")
    # thicken the descending curve of the loop a little (the heavy stroke of a clef)
    d.line(pts([(1.6 * u, 25 * u), (2.6 * u, 19 * u)]), fill=255, width=lw + S // 2)
    d.ellipse(pts([(2.0 * u, 37.4 * u), (4.6 * u, 40.0 * u)]), fill=255)
    return w, h, reduce(im, w, h)


def sharp(sp):
    u = sp / 6.0
    w, h = int(round(7 * u)) | 1, int(round(15 * u))
    im, d = canvas(w, h)
    lw = max(1, int(round(1.1 * u * S)))
    d.line(pts([(2.2 * u, 1 * u), (2.2 * u, h - 0.3)]), fill=255, width=lw)
    d.line(pts([(4.8 * u, 0.3 * u), (4.8 * u, h - 1 * u)]), fill=255, width=lw)
    tw = int(round(2.2 * u * S))
    d.line(pts([(0.2, 5.6 * u), (w - 0.2, 4.2 * u)]), fill=255, width=tw)
    d.line(pts([(0.2, 10.6 * u), (w - 0.2, 9.2 * u)]), fill=255, width=tw)
    return w, h, reduce(im, w, h)


def flat(sp):
    u = sp / 6.0
    w, h = int(round(6 * u)), int(round(14 * u))
    im, d = canvas(w, h)
    lw = max(1, int(round(1.2 * u * S)))
    d.line(pts([(1.0 * u, 0), (1.0 * u, h - 0.5)]), fill=255, width=lw)
    belly = [(1.0 * u, 8.0 * u), (3.0 * u, 6.6 * u), (5.0 * u, 7.4 * u), (4.6 * u, 10.0 * u), (1.0 * u, 13.4 * u)]
    d.line(pts(belly), fill=255, width=lw + S // 3, joint="curve")
    return w, h, reduce(im, w, h)


def natural(sp):
    u = sp / 6.0
    w, h = int(round(5 * u)), int(round(15 * u))
    im, d = canvas(w, h)
    lw = max(1, int(round(1.1 * u * S)))
    d.line(pts([(0.8 * u, 0.3), (0.8 * u, 10.5 * u)]), fill=255, width=lw)
    d.line(pts([(4.0 * u, 4.5 * u), (4.0 * u, h - 0.3)]), fill=255, width=lw)
    tw = int(round(2.0 * u * S))
    d.line(pts([(0.8 * u, 5.6 * u), (4.0 * u, 4.6 * u)]), fill=255, width=tw)
    d.line(pts([(0.8 * u, 10.4 * u), (4.0 * u, 9.4 * u)]), fill=255, width=tw)
    return w, h, reduce(im, w, h)


def qrest(sp):
    u = sp / 6.0
    w, h = int(round(6 * u)), int(round(16 * u))
    im, d = canvas(w, h)
    lw = max(1, int(round(1.4 * u * S)))
    z = [(1.5 * u, 0.3), (4.8 * u, 4.5 * u), (1.6 * u, 8.0 * u), (4.8 * u, 11.0 * u), (2.0 * u, 11.5 * u),
         (1.4 * u, 13.5 * u), (3.4 * u, 15.6 * u)]
    d.line(pts(z), fill=255, width=lw, joint="curve")
    d.line(pts([(1.6 * u, 8.0 * u), (4.0 * u, 5.5 * u)]), fill=255, width=lw + S // 2)
    return w, h, reduce(im, w, h)


def erest(sp, flags=1):
    u = sp / 6.0
    w, h = int(round(7 * u)), int(round((9 + 4 * (flags - 1)) * u))
    im, d = canvas(w, h)
    lw = max(1, int(round(1.1 * u * S)))
    d.line(pts([(6.2 * u, 0.5 * u), (2.6 * u, h - 0.3)]), fill=255, width=lw)
    for k in range(flags):
        y = (2.2 + 4 * k) * u
        x = 6.0 * u - k * 1.3 * u
        d.ellipse(pts([(x - 5.4 * u, y - 1.6 * u), (x - 2.6 * u, y + 1.2 * u)]), fill=255)
        d.line(pts([(x - 4.0 * u, y + 0.8 * u), (x, y - 1.4 * u)]), fill=255, width=lw)
    return w, h, reduce(im, w, h)


GLYPHS = [("clef", clef), ("sharp", sharp), ("flat", flat), ("natural", natural), ("qrest", qrest),
          ("erest", lambda sp: erest(sp, 1)), ("srest", lambda sp: erest(sp, 2))]


def to_c(name, w, h, rows):
    rb = ((w + 15) // 16) * 2
    data = []
    for r in rows:
        bits = r + [0] * (rb * 8 - w)
        for b in range(rb):
            v = 0
            for k in range(8):
                v = (v << 1) | bits[b * 8 + k]
            data.append(v)
    return "static const unsigned char g_%s[] = {%s};\n" % (name, ",".join("0x%02X" % v for v in data)), (w, h, rb)


def main():
    out = ["/* Generated by gen_glyphs.py: Tune's music glyphs (original drawings), 1 bit. */",
           '#include "glyphs.h"']
    table = []
    previews = []
    for size, sp in (("big", 6), ("small", 4)):
        for name, fn in GLYPHS:
            w, h, rows = fn(sp)
            c, (w, h, rb) = to_c(name + "_" + size, w, h, rows)
            out.append(c)
            table.append("    {g_%s_%s, %d, %d, %d}" % (name, size, w, h, rb))
            previews.append(rows)
    out.append("const Glyph glyphs[2][%d] = {\n%s\n};" % (len(GLYPHS), ",\n".join(
        ["{" + ",".join(table[i * len(GLYPHS):(i + 1) * len(GLYPHS)]) + "}" for i in range(2)])))
    open(os.path.join(HERE, "glyphs.c"), "w").write("\n".join(out) + "\n")
    hdr = ["/* Generated by gen_glyphs.py. */", "#ifndef GLYPHS_H", "#define GLYPHS_H",
           "typedef struct Glyph { const unsigned char *bits; short w, h, rowBytes; } Glyph;",
           "enum { " + ", ".join("GL_" + n.upper() for n, _ in GLYPHS) + ", GL_N };",
           "extern const Glyph glyphs[2][GL_N];   /* [0] line spacing 6, [1] line spacing 4 */",
           "#endif"]
    open(os.path.join(HERE, "glyphs.h"), "w").write("\n".join(hdr) + "\n")
    if "--preview" in sys.argv:
        p = sys.argv[sys.argv.index("--preview") + 1]
        W = sum(len(r[0]) + 4 for r in previews)
        H = max(len(r) for r in previews)
        im = Image.new("L", (W, H), 255)
        x = 0
        for rows in previews:
            for y, r in enumerate(rows):
                for i, v in enumerate(r):
                    if v:
                        im.putpixel((x + i, y), 0)
            x += len(rows[0]) + 4
        im.resize((W * 6, H * 6), Image.NEAREST).save(p)
    print("ok")


if __name__ == "__main__":
    main()

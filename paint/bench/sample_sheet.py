#!/usr/bin/env python3
"""Sample sheet for judging a model the way the Mac will draw it: everything here is computed
with the bit-exact integer reference (ref.py), i.e. the same numbers the 68000 code produces.

  sample_sheet.py <model.dmdl> <out.png> [--temp 1] [--seed 1]

Top: 24 categories x 2 drawings sampled from scratch (temperature 0.5 by default).
Bottom: 6 partial drawings from Google's Quick, Draw! dataset (CC BY 4.0; data/partials.json,
made by fetch_partials.py) finished by the model: prompt in black, completion in blue, inside
the 256x256 AI frame, as Doodle's "Finish it" would draw them.
"""
import argparse
import json
import os
import sys

from PIL import Image, ImageDraw

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import model_file  # noqa: E402
import ref  # noqa: E402

CATS = ["cat", "dog", "bicycle", "house", "tree", "fish", "car", "face", "flower", "airplane",
        "The Eiffel Tower", "bird", "butterfly", "snowman", "sun", "apple", "clock", "guitar",
        "rabbit", "umbrella", "star", "cup", "pig", "sailboat"]
BLUE, GRAY = (0, 80, 230), (190, 190, 190)


def walk(gen, x, y, pen):
    """Segments drawn by generated rows from (x, y) with the previous pen state."""
    segs = []
    for dx, dy, p in gen:
        if pen == 0:
            segs.append((x, y, x + dx, y + dy))
        x, y, pen = x + dx, y + dy, p
    return segs, x, y


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("out")
    ap.add_argument("--temp", type=int, default=1, help="0 tidy 0.3, 1 normal 0.5, 2 wild 0.8")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--mix", type=float, default=0, help="mixture temperature (overrides --temp)")
    ap.add_argument("--pen", type=float, default=0, help="pen temperature (with --mix)")
    a = ap.parse_args()
    m = model_file.read(a.model)
    names = m["names"]
    cell, lab = 128, 14
    W = 8 * cell
    top_h = 6 * (cell + lab)
    part = 2 * (256 + 2 * lab + 8)
    img = Image.new("RGB", (W, 30 + top_h + 30 + part), "white")
    d = ImageDraw.Draw(img)
    if a.mix:
        temp = (a.mix, a.pen or a.mix)
    else:
        temp = a.temp
    tm, tp = ref.TEMP_SETTINGS[temp] if isinstance(temp, int) else temp
    tname = f"mixture {tm}, pen {tp}" + (f" (Finish menu setting {temp})" if isinstance(temp, int) else "")
    d.text((4, 4), f"{os.path.basename(a.model)}: H={m['H']}, sampled with the Mac's integer arithmetic "
                   f"(paint/bench/ref.py), temperature {tname}", fill="black")
    y0 = 30
    for i, cat in enumerate(CATS):
        if cat not in names:
            continue
        c = names.index(cat)
        col, row = (i % 4) * 2, i // 4
        x0, yy = col * cell, y0 + row * (cell + lab)
        d.text((x0 + 3, yy), cat, fill="black")
        for j in range(2):
            gen = ref.finish_it(m, [], c, a.seed * 7919 + 101 * i + j + 1, temp)
            segs, _, _ = walk(gen, 0, 0, 0)
            xs = [0] + [v for s in segs for v in (s[0], s[2])]
            ys = [0] + [v for s in segs for v in (s[1], s[3])]
            span = max(max(xs) - min(xs), max(ys) - min(ys), 1)
            sc = (cell - 12) / span
            ox, oy = x0 + j * cell + 6 - min(xs) * sc, yy + lab + 6 - min(ys) * sc
            d.rectangle((x0 + j * cell + 1, yy + lab, x0 + j * cell + cell - 2, yy + lab + cell - 2), outline=GRAY)
            for s in segs:
                d.line((ox + s[0] * sc, oy + s[1] * sc, ox + s[2] * sc, oy + s[3] * sc), fill="black")
    py = y0 + top_h + 8
    d.text((4, py), "Finish it on partial Quick, Draw! drawings (CC BY 4.0): prompt black, the model's "
                    "completion blue, in the 256x256 AI frame (full size; strokes outside the frame are clipped as on the Mac)",
           fill="black")
    parts = json.load(open(os.path.join(HERE, "data", "partials.json")))["partials"]
    for k, p in enumerate(parts[:6]):
        if p["category"] not in names:
            continue
        pts = []
        for xs, ys in p["strokes"]:
            for t in range(len(xs)):
                pts.append((xs[t], ys[t], 1 if t == len(xs) - 1 else 0))
        res = ref.prompt_rows(pts)
        if res is None:
            continue
        (sx, sy), rows = res
        gen = ref.finish_it(m, rows, names.index(p["category"]), a.seed * 31 + k + 1, temp)
        psegs, x, y = walk(rows, sx, sy, 0)
        gsegs, _, _ = walk(gen, x, y, rows[-1][2])
        bx, by = 4 + (k % 3) * 300, py + 22 + (k // 3) * (256 + 2 * lab + 8)
        d.text((bx, by), f"{p['category']}: {len(rows)} prompt rows + {len(gen)} generated", fill="black")
        frame = Image.new("RGB", (256, 256), "white")       # drawing outside it is cut off, like ClipRect
        fd = ImageDraw.Draw(frame)
        for segs, colr in ((psegs, "black"), (gsegs, BLUE)):
            for s_ in segs:
                fd.line(s_, fill=colr, width=1)
        img.paste(frame, (bx, by + lab))
        d.rectangle((bx - 1, by + lab - 1, bx + 256, by + lab + 256), outline=GRAY)
    img.save(a.out)
    print("wrote", a.out)


if __name__ == "__main__":
    main()

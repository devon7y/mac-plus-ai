#!/usr/bin/env python3
"""Contact sheet of a showcase run: showcase_sheet.py RUN_DIR [OUT.png] [TITLE]
(RUN_DIR/labels.txt lists 'file<TAB>label'; the shots are in RUN_DIR/shots/). Each shot is the
whole 512 x 342 screen (the Full view size), labelled; 4 per row."""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

run = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "showcase.png")
items = [ln.rstrip("\n").split("\t", 1) for ln in open(os.path.join(run, "labels.txt")) if "\t" in ln]
CROP = (0, 0, 512, 342)                      # the whole screen: the view, the status row, the hotbar
W, H = CROP[2] - CROP[0], CROP[3] - CROP[1]
COLS, LAB, PAD = 4, 34, 10
rows = (len(items) + COLS - 1) // COLS
sheet = Image.new("RGB", (COLS * (W + PAD) + PAD, 44 + rows * (H + LAB + PAD)), (236, 236, 236))
d = ImageDraw.Draw(sheet)
try:
    font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 13)
    big = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 20)
except OSError:
    font = big = ImageFont.load_default()
TITLE = sys.argv[3] if len(sys.argv) > 3 else "Blockworld on a 4 MB Mac Plus (MAME, real speed): showcase"
d.text((PAD, 10), TITLE, fill=(0, 0, 0), font=big)


def wrap(text, width):
    lines, cur = [], ""
    for w in text.split():
        t = (cur + " " + w).strip()
        if d.textlength(t, font=font) > width and cur:
            lines.append(cur)
            cur = w
        else:
            cur = t
    return lines + [cur]


for i, (f, label) in enumerate(items):
    r, c = divmod(i, COLS)
    x, y = PAD + c * (W + PAD), 44 + r * (H + LAB + PAD)
    im = Image.open(os.path.join(run, "shots", f)).convert("RGB").crop(CROP)
    sheet.paste(im, (x, y))
    for k, ln in enumerate(wrap(f"{i + 1}. {label}", W)[:2]):
        d.text((x, y + H + 3 + 15 * k), ln, fill=(0, 0, 0), font=font)
sheet.save(out)
print(out, sheet.size, len(items), "shots")

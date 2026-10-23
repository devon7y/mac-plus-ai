#!/usr/bin/env python3
"""Contact sheet: sheet.py OUT.png COLS img1.png img2.png ... (images shown at half size)."""
import sys
from PIL import Image
out, cols, files = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
ims = [Image.open(f).convert("L") for f in files]
ims = [im.resize((im.width // 2, im.height // 2), Image.NEAREST) for im in ims]
w, h = max(i.width for i in ims), max(i.height for i in ims)
rows = (len(ims) + cols - 1) // cols
sheet = Image.new("L", (cols * (w + 6), rows * (h + 6)), 128)
for n, im in enumerate(ims):
    sheet.paste(im, ((n % cols) * (w + 6), (n // cols) * (h + 6)))
sheet.save(out)

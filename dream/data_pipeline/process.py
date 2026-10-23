"""Reference post-processing for Dream training images (this file defines the data format).

512x512 RGB from FLUX -> greyscale -> near-white pushed to white -> crop to the subject's bounding
box (non-white pixels) plus a margin -> pad to a white square with the subject centred ->
area-average down to 64x64 and (directly from the padded square) to 32x32 -> quantize to 16 grey
levels, uint8 0..15 with 0 = black and 15 = white.

Split files (written by filter.py select, or by write_split below):
  images64.npy (N,64,64) uint8 0..15, images32.npy (N,32,32) uint8 0..15,
  labels.npy (N,) int16 index into subjects.txt, meta.jsonl (prompt, seed, subject, source, scores).
"""
import json
import os

import numpy as np
from PIL import Image

WHITE = 0.90      # grey values above this become pure white (ink = grey <= WHITE)
MARGIN = 0.06     # margin around the subject's bounding box, as a fraction of its longer side
LEVELS = 16


def to_grey(rgb):
    """uint8 (H,W,3) or (H,W) -> float32 grey in [0,1] (ITU-R 601 luma)."""
    a = np.asarray(rgb, dtype=np.float32) / 255.0
    if a.ndim == 3:
        a = a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114
    return a


def whiten(g, white=WHITE):
    g = g.copy()
    g[g > white] = 1.0
    return g


def bbox(ink):
    ys = np.flatnonzero(ink.any(1))
    xs = np.flatnonzero(ink.any(0))
    if len(ys) == 0:
        return None
    return ys[0], ys[-1] + 1, xs[0], xs[-1] + 1     # y0, y1, x0, x1 (exclusive)


def crop_square(g, white=WHITE, margin=MARGIN):
    """Crop the whitened grey image to the ink bounding box plus margin, pad to a centred square."""
    b = bbox(g <= white)
    if b is None:
        return np.ones((64, 64), np.float32)
    y0, y1, x0, x1 = b
    h, w = y1 - y0, x1 - x0
    side = max(h, w)
    m = int(round(margin * side))
    S = side + 2 * m
    sq = np.ones((S, S), np.float32)
    oy, ox = (S - h) // 2, (S - w) // 2
    sq[oy:oy + h, ox:ox + w] = g[y0:y1, x0:x1]
    return sq


def area_resize(sq, n):
    """Exact area averaging (PIL BOX filter on float data)."""
    return np.asarray(Image.fromarray(sq.astype(np.float32), mode='F').resize((n, n), Image.BOX),
                      dtype=np.float32)


def quantize(g, levels=LEVELS):
    return np.clip(np.rint(g * (levels - 1)), 0, levels - 1).astype(np.uint8)


def process(rgb, white=WHITE, margin=MARGIN):
    """512x512 RGB uint8 -> (img64, img32) uint8 0..15 (0 = black)."""
    g = whiten(to_grey(rgb), white)
    sq = crop_square(g, white, margin)
    return quantize(area_resize(sq, 64)), quantize(area_resize(sq, 32))


def to_u8(img16):
    """0..15 -> 0..255 for viewing."""
    return (img16.astype(np.uint16) * 17).astype(np.uint8)


def write_split(out, img64, img32, labels, metas):
    os.makedirs(out, exist_ok=True)
    import shutil
    shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'subjects.txt'), os.path.join(out, 'subjects.txt'))
    np.save(os.path.join(out, 'images64.npy'), np.asarray(img64, np.uint8))
    np.save(os.path.join(out, 'images32.npy'), np.asarray(img32, np.uint8))
    np.save(os.path.join(out, 'labels.npy'), np.asarray(labels, np.int16))
    with open(os.path.join(out, 'meta.jsonl'), 'w') as f:
        for m in metas:
            f.write(json.dumps(m) + '\n')


if __name__ == '__main__':
    # Process one PNG for a quick look:  python process.py in.png out_prefix
    import sys
    rgb = np.asarray(Image.open(sys.argv[1]).convert('RGB'))
    a64, a32 = process(rgb)
    Image.fromarray(to_u8(a64)).resize((256, 256), Image.NEAREST).save(sys.argv[2] + '_64.png')
    Image.fromarray(to_u8(a32)).resize((256, 256), Image.NEAREST).save(sys.argv[2] + '_32.png')

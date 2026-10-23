"""Contact sheets for reviewing Dream data.

  python contact.py --split data/pilot/split --feat data/pilot/feat --out sheets/ [--per-row 16]

Writes contact_64.png (one row per subject, kept 64x64 images at 2x, nearest neighbour, 16 greys),
contact_32.png (same images at 32x32, 3x), rejected.png (raw thumbnails of a stratified sample of
rejected images with reason codes), review_kept.png (100 random kept: raw thumbnail + 64x64) and
review_rejected.png (50 random rejected: raw thumbnail + 64x64 + reasons).
"""
import argparse
import json
import os
import random

import numpy as np
from PIL import Image, ImageDraw, ImageFont

import process

BG, FG = 200, 0


def font(size=12):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def tile16(img, scale):
    return Image.fromarray(process.to_u8(img)).resize((img.shape[1] * scale, img.shape[0] * scale), Image.NEAREST)


def rows_sheet(rows, scale, label_w=110, gap=4, per_line=1, sep=16):
    """rows: list of (label, [img16...]); per_line subjects side by side on each line."""
    n = max(len(r[1]) for r in rows)
    side = rows[0][1][0].shape[0] * scale
    block = label_w + n * (side + gap) + sep
    lines = [rows[k:k + per_line] for k in range(0, len(rows), per_line)]
    sheet = Image.new('L', (per_line * block + gap, len(lines) * (side + gap) + gap), BG)
    dr = ImageDraw.Draw(sheet); f = font(13)
    for r, line in enumerate(lines):
        y = gap + r * (side + gap)
        for b, (lab, imgs) in enumerate(line):
            x0 = b * block
            dr.text((x0 + 6, y + side // 2 - 7), lab, fill=FG, font=f)
            for c, im in enumerate(imgs):
                sheet.paste(tile16(im, scale), (x0 + label_w + gap + c * (side + gap), y))
    return sheet


def save_pages(rows, scale, path, per_line, lines_per_page):
    """One sheet, or pages <path>_p1.png ... when lines_per_page is set."""
    if not lines_per_page:
        rows_sheet(rows, scale, per_line=per_line).save(path)
        return
    k = per_line * lines_per_page
    for p in range(0, len(rows), k):
        rows_sheet(rows[p:p + k], scale, per_line=per_line).save(path.replace('.png', f'_p{p // k + 1}.png'))


def grid(cells, cols, cell_w, cell_h, gap=4):
    rows = (len(cells) + cols - 1) // cols
    sheet = Image.new('L', (cols * (cell_w + gap) + gap, rows * (cell_h + gap) + gap), BG)
    for k, c in enumerate(cells):
        sheet.paste(c, (gap + (k % cols) * (cell_w + gap), gap + (k // cols) * (cell_h + gap)))
    return sheet


def feat_lookup(feat_dir):
    cache = {}

    def get(meta):
        slug = os.path.basename(meta['source'].split(':')[0])[:-4]
        if slug not in cache:
            d = np.load(os.path.join(feat_dir, slug + '.npz'))
            cache[slug] = (dict((int(s), k) for k, s in enumerate(d['seeds'])), d['thumb'], d['img64'])
        idx, th, i64 = cache[slug]
        k = idx[meta['seed']]
        return th[k], i64[k]
    return get


def cell(thumb, img64, lines, w=128 + 4 + 128):
    f = font(11)
    c = Image.new('L', (w, 128 + 14 * len(lines) + 2), 255)
    c.paste(Image.fromarray(thumb), (0, 0))
    if img64 is not None:
        c.paste(tile16(img64, 2), (132, 0))
    dr = ImageDraw.Draw(c)
    for i, l in enumerate(lines):
        dr.text((2, 129 + 14 * i), l, fill=0, font=f)
    return c


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--split', required=True); ap.add_argument('--feat', required=True)
    ap.add_argument('--out', required=True); ap.add_argument('--per-row', type=int, default=16)
    ap.add_argument('--seed', type=int, default=0)
    ap.add_argument('--min-i', type=int, default=0, help='only images with index i >= this')
    ap.add_argument('--per-line', type=int, default=1, help='subjects side by side per line')
    ap.add_argument('--lines-per-page', type=int, default=0, help='paginate the contact sheets')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    rnd = random.Random(a.seed)
    i64 = np.load(os.path.join(a.split, 'images64.npy')); i32 = np.load(os.path.join(a.split, 'images32.npy'))
    metas = [json.loads(l) for l in open(os.path.join(a.split, 'meta.jsonl'))]
    rej = [json.loads(l) for l in open(os.path.join(a.split, 'rejected.jsonl'))]
    if a.min_i:                          # e.g. only the images added after the pilot (i >= 70)
        keep = [k for k, m in enumerate(metas) if m['i'] >= a.min_i]
        i64, i32, metas = i64[keep], i32[keep], [metas[k] for k in keep]
        rej = [r for r in rej if r['i'] >= a.min_i]
    subjects = []
    for m in metas + rej:
        if m['subject'] not in subjects:
            subjects.append(m['subject'])
    by = {s: [] for s in subjects}
    for k, m in enumerate(metas):
        by[m['subject']].append(k)
    pick = {s: sorted(rnd.sample(v, min(a.per_row, len(v)))) for s, v in by.items()}
    lab = lambda s: f'{s} ({len(by[s])})'
    rows64 = [(lab(s), [i64[k] for k in pick[s]]) for s in subjects if pick[s]]
    rows32 = [(lab(s), [i32[k] for k in pick[s]]) for s in subjects if pick[s]]
    save_pages(rows64, 2, os.path.join(a.out, 'contact_64.png'), a.per_line, a.lines_per_page)
    save_pages(rows32, 3, os.path.join(a.out, 'contact_32.png'), a.per_line, a.lines_per_page)

    get = feat_lookup(a.feat)
    # rejected sample: stratified by primary reason, up to 8 per reason
    byr = {}
    for r in rej:
        byr.setdefault(r['primary'], []).append(r)
    samp = []
    for k in sorted(byr, key=lambda k: -len(byr[k])):
        samp += rnd.sample(byr[k], min(8, len(byr[k])))
    cells = [cell(*get(r), [f"{r['subject']} {r['seed']}", ','.join(r['reasons'])]) for r in samp]
    if cells:
        grid(cells, 6, cells[0].width, cells[0].height).save(os.path.join(a.out, 'rejected.png'))
    # honest review: uniform random samples
    ks = rnd.sample(range(len(metas)), min(100, len(metas)))
    cells = [cell(*get(metas[k]), [f"{metas[k]['subject']} {metas[k]['seed']} r{metas[k]['scores']['rank']}"]) for k in ks]
    for p in range(0, len(cells), 50):
        grid(cells[p:p + 50], 5, cells[0].width, cells[0].height).save(os.path.join(a.out, f'review_kept_{p // 50}.png'))
    # near-duplicate pairs: dropped image (left) next to the kept image it duplicates (right)
    dups = [r for r in rej if 'dup_of' in r]
    cells = []
    for r in rnd.sample(dups, min(30, len(dups))):
        t1, _ = get(r)
        t2, _ = get(dict(r, seed=r['dup_of']))
        cells.append(cell(t1, None, [f"{r['subject']} {r['seed']} = {r['dup_of']}"], w=260))
        cells[-1].paste(Image.fromarray(t2), (132, 0))
    if cells:
        grid(cells, 5, cells[0].width, cells[0].height).save(os.path.join(a.out, 'dup_pairs.png'))
    rs = rnd.sample(rej, min(50, len(rej)))
    cells = [cell(*get(r), [f"{r['subject']} {r['seed']}", ','.join(r['reasons'])]) for r in rs]
    if cells:
        grid(cells, 5, cells[0].width, cells[0].height).save(os.path.join(a.out, 'review_rejected.png'))
    print('sheets written to', a.out)


if __name__ == '__main__':
    main()

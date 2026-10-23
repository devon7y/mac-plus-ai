"""Side-by-side generator comparison sheets.

  python compare.py --models schnell=data/compare/schnell/raw klein=data/compare/klein/raw --out sheets/

For each model: <label>_512.png (rows = subjects, the generated 512x512 images as 128 px
thumbnails, in colour if any) and <label>_64.png (process.py result, 64x64 at 2x, 16 greys).
Combined: compare_512.png and compare_64.png with the models side by side per subject.
"""
import argparse
import io
import json
import os
import tarfile

import numpy as np
from PIL import Image, ImageDraw

import process
import prompts
from contact import font

BG = (200, 200, 200)


def load(raw):
    """-> {subject: [(seed, rgb uint8)]} in seed order."""
    out = {}
    for fn in sorted(f for f in os.listdir(raw) if f.endswith('.tar')):
        metas = {json.loads(l)['seed']: json.loads(l) for l in open(os.path.join(raw, fn[:-4] + '.jsonl')) if l.strip()}
        with tarfile.open(os.path.join(raw, fn)) as tf:
            for mem in tf.getmembers():
                seed = int(mem.name.split('.')[0])
                rgb = np.asarray(Image.open(io.BytesIO(tf.extractfile(mem).read())).convert('RGB'))
                out.setdefault(metas[seed]['subject'], []).append((seed, rgb))
    return {s: sorted(v, key=lambda x: x[0]) for s, v in out.items()}


def thumb512(rgb, side):
    return Image.fromarray(rgb).resize((side, side), Image.BOX)


def thumb64(rgb):
    a64, _ = process.process(rgb)
    return Image.fromarray(process.to_u8(a64)).resize((128, 128), Image.NEAREST).convert('RGB')


def sheet(subjects, groups, side, label_w=110, gap=4, sep=16, header=None):
    """groups: list of {subject: [PIL tiles]} drawn left to right with a wider gap between groups."""
    n = [max(len(g.get(s, [])) for s in subjects) for g in groups]
    W = label_w + sum(k * (side + gap) for k in n) + sep * (len(groups) - 1) + gap
    top = 20 if header else 0
    H = top + len(subjects) * (side + gap) + gap
    im = Image.new('RGB', (W, H), BG)
    dr = ImageDraw.Draw(im); f = font(13)
    if header:
        x = label_w + gap
        for h, k in zip(header, n):
            dr.text((x, 3), h, fill=(0, 0, 0), font=f); x += k * (side + gap) + sep
    for r, s in enumerate(subjects):
        y = top + gap + r * (side + gap)
        dr.text((6, y + side // 2 - 7), s, fill=(0, 0, 0), font=f)
        x = label_w + gap
        for g, k in zip(groups, n):
            for c, t in enumerate(g.get(s, [])):
                im.paste(t, (x + c * (side + gap), y))
            x += k * (side + gap) + sep
    return im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--models', nargs='+', required=True, help='label=raw_dir')
    ap.add_argument('--out', required=True)
    ap.add_argument('--subjects', default=os.path.join(prompts.HERE, 'pilot_subjects.txt'))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    subjects = prompts.read_list(a.subjects)
    labels, data = [], []
    for m in a.models:
        lab, d = m.split('=', 1)
        labels.append(lab); data.append(load(d))
    g512 = [{s: [thumb512(rgb, 128) for _, rgb in d.get(s, [])] for s in subjects} for d in data]
    g64 = [{s: [thumb64(rgb) for _, rgb in d.get(s, [])] for s in subjects} for d in data]
    for lab, a5, a6 in zip(labels, g512, g64):
        sheet(subjects, [a5], 128, header=[lab]).save(os.path.join(a.out, f'{lab}_512.png'))
        sheet(subjects, [a6], 128, header=[lab + ' (64x64 at 2x)']).save(os.path.join(a.out, f'{lab}_64.png'))
    small = [{s: [t.resize((96, 96), Image.BOX) for t in g[s]] for s in subjects} for g in g512]
    sheet(subjects, small, 96, header=labels).save(os.path.join(a.out, 'compare_512.png'))
    sheet(subjects, g64, 128, header=[l + ' (64x64 at 2x)' for l in labels]).save(os.path.join(a.out, 'compare_64.png'))
    print('sheets written to', a.out)


if __name__ == '__main__':
    main()

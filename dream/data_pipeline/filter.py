"""Score and filter Dream training images.

  score  (GPU): for each <raw>/<slug>.tar, compute heuristics on the 512x512 image, the processed
                64x64/32x32 images (process.py), a 128x128 grey thumbnail, the NSFW classifier score
                (Falconsai/nsfw_image_detection, the content filter BFL's reference CLI loads), and
                OpenCLIP ViT-H-14 embeddings of the full image and of the 64x64 result; writes
                <feat>/<slug>.npz and <feat>/text.npz (class and negative text embeddings).
  select (CPU): apply thresholds, drop near-duplicates, write the split (process.write_split) plus
                rejected.jsonl and select_stats.json. Every rejection records all failed rules.

  python filter.py score  --raw data/pilot/raw --feat data/pilot/feat --clip models/CLIP-ViT-H-14-laion2B-s32B-b79K \
                         --nsfw models/nsfw-classifier
  python filter.py select --feat data/pilot/feat --out data/pilot/split [--th key=value ...]
"""
import argparse
import io
import json
import os
import tarfile
import time

import numpy as np
from PIL import Image

import process
import prompts

CLASS_TEMPLATES = ['a black and white clip art of {a} {s}.', 'a simple drawing of {a} {s}.',
                   'an icon of {a} {s}.', '{a} {s}.']
NEGATIVES = {
    'text': ['text', 'a page of text', 'letters and words', 'a logo with text', 'a label with writing on it'],
    'frame': ['a picture inside a frame', 'a rectangular border', 'a decorative frame', 'a badge or emblem'],
    'photo': ['a photograph', 'a realistic photo', 'a colorful painting'],
    'many': ['many different objects', 'a repeated pattern', 'a collage of several pictures', 'a busy scene'],
    'dark': ['a white drawing on a black background'],
    'unsafe': ['nudity', 'a violent or gory image', 'a weapon pointed at a person'],
}
GENERIC_POS = ['a black and white clip art of a single object on a white background']

# Thresholds (override with --th name=value). Tuned on the M0 pilot by looking at contact sheets.
TH = dict(
    nsfw=0.85,           # NSFW classifier 'nsfw' probability above this -> 'nsfw' (BFL reference threshold)
    p_unsafe=0.5,        # CLIP probability of the 'unsafe' negatives above this -> 'unsafe'
    border_white=0.97,   # outer 16 px strip must be at least this white -> else 'bg'
    edge_ink=0.002,      # fraction of ink in the outer 3 px ring above this -> 'edge' (subject cut off)
    ink_min=0.012,       # ink fraction of the 512 image below this -> 'faint'
    fill_max=0.90,       # dark (<0.35) fraction inside the bbox above this -> 'heavy' (0.8 rejected solid black doors)
    multi_n=99,          # component rule, OFF: on the M0 pilot it only ever flagged single objects (sails apart
    multi_second=1.01,   # from hulls, flower centres, 'sticker' outer contours); CLIP 'many' covers multi-object
    colour_frac=0.30,    # fraction of ink pixels with chroma > 48 above this -> 'colour'
    frame_cov=0.90,      # all four bbox sides covered by ink at least this much -> 'frame', but only for subjects
    frame_subject_median=0.5,  # whose median frame_cov is below this (doors, washing machines are boxes by nature)
    rank_max=5,          # true subject must rank within top-k among subjects.txt (full image) -> 'nomatch'
    p_min=0.10,          # ... and if it is not the top-1 subject, its zero-shot probability must reach p_min (M2
                         # review: below this, 'trucks' were vans, 'trains' buses, 'necklaces' crescents)
    rank64_max=20,       # ... and within top-k for the 64x64 result -> 'lost64'
    p_text=0.5, p_frame=0.6, p_photo=0.6, p_many=0.6,   # negative-text probabilities
    p_dark=1.01,         # OFF: 'white drawing on black' fired on ordinary dragons; border_white catches dark backgrounds
    dup_cos=0.97,        # near-copy of an already kept image of the same subject -> 'dup' when the CLIP cosine
    dup_mad=0.03,        # is above dup_cos AND the mean |difference| of the 32x32 images (0..1) is below dup_mad
)
EXCLUDE_PROMPT = ['bite taken out']   # M2 probe: bitten apples looked like a company logo -> 'excluded'
ORDER = ['excluded', 'nsfw', 'unsafe', 'bg', 'edge', 'faint', 'heavy', 'colour', 'frame', 'text', 'photo', 'dark', 'many', 'multi',
         'nomatch', 'lost64', 'dup']


# ----------------------------------------------------------------------------------------- score
def heuristics(rgb):
    from scipy import ndimage
    g = process.to_grey(rgb)
    ink = g <= process.WHITE
    H, W = g.shape
    b = 16
    strip = np.ones_like(ink); strip[b:-b, b:-b] = False
    ring = np.ones_like(ink); ring[3:-3, 3:-3] = False
    f = dict(border_white=1.0 - ink[strip].mean(), edge_ink=float(ink[ring].mean()),
             ink_frac=float(ink.mean()), dark_frac=float((g < 0.35).mean()))
    bb = process.bbox(ink)
    if bb is None:
        f.update(bbox_h=0., bbox_w=0., fill=0., dark_fill=0., n_cc5=0, second=0., second_frac=0., frame_cov=0.,
                 colour_frac=0., grey_frac=0.)
        return f
    y0, y1, x0, x1 = bb
    box = ink[y0:y1, x0:x1]
    f.update(bbox_h=(y1 - y0) / H, bbox_w=(x1 - x0) / W, fill=float(box.mean()),
             dark_fill=float((g[y0:y1, x0:x1] < 0.35).mean()))
    lab, n = ndimage.label(ndimage.binary_dilation(ink, iterations=3))
    sizes = np.bincount(lab[ink], minlength=n + 1)[1:]
    sizes = np.sort(sizes)[::-1]
    tot = max(1, sizes.sum())
    f['n_cc5'] = int((sizes >= 0.05 * tot).sum())
    f['second'] = float(sizes[1] / sizes[0]) if len(sizes) > 1 else 0.0
    f['second_frac'] = float(sizes[1] / tot) if len(sizes) > 1 else 0.0
    d = 8
    cov = [box[:d].any(0).mean(), box[-d:].any(0).mean(), box[:, :d].any(1).mean(), box[:, -d:].any(1).mean()]
    f['frame_cov'] = float(min(cov))
    rgbi = np.asarray(rgb, np.int16)
    chroma = rgbi.max(-1) - rgbi.min(-1)
    f['colour_frac'] = float((chroma[ink] > 48).mean())
    f['grey_frac'] = float(((g > 0.3) & (g <= 0.8))[ink].mean())
    return f


def article_text(t, s):
    p = prompts.phrase(s)
    return t.format(a=prompts.article(p), s=p).replace('a the ', 'the ').replace('an the ', 'the ')


def load_clip(path, device):
    import open_clip
    import torch
    model, _, pre = open_clip.create_model_and_transforms(
        'ViT-H-14', pretrained=os.path.join(path, 'open_clip_model.safetensors'), device=device)
    model.eval()
    tok = open_clip.get_tokenizer('ViT-H-14')
    return model, pre, tok


def encode_texts(model, tok, texts, device):
    import torch
    with torch.no_grad(), torch.autocast('cuda', dtype=torch.bfloat16):
        e = model.encode_text(tok(texts).to(device)).float()
    return torch.nn.functional.normalize(e, dim=-1).cpu().numpy()


def encode_images(model, pre, pils, device, bs=64):
    import torch
    out = []
    for k in range(0, len(pils), bs):
        x = torch.stack([pre(p) for p in pils[k:k + bs]]).to(device)
        with torch.no_grad(), torch.autocast('cuda', dtype=torch.bfloat16):
            e = model.encode_image(x).float()
        out.append(torch.nn.functional.normalize(e, dim=-1).cpu().numpy())
    return np.concatenate(out).astype(np.float16)


def load_nsfw(path, device):
    from transformers import AutoImageProcessor, AutoModelForImageClassification
    proc = AutoImageProcessor.from_pretrained(path)
    model = AutoModelForImageClassification.from_pretrained(path).to(device).eval()
    idx = [k for k, v in model.config.id2label.items() if v.lower() == 'nsfw'][0]
    return model, proc, int(idx)


def nsfw_scores(nsfw, pils, device, bs=64):
    import torch
    model, proc, idx = nsfw
    out = []
    for k in range(0, len(pils), bs):
        x = proc(images=pils[k:k + bs], return_tensors='pt').to(device)
        with torch.no_grad():
            out.append(torch.softmax(model(**x).logits.float(), -1)[:, idx].cpu().numpy())
    return np.concatenate(out).astype(np.float32)


def score(a):
    device = 'cuda'
    model, pre, tok = load_clip(a.clip, device)
    nsfw = load_nsfw(a.nsfw, device)
    os.makedirs(a.feat, exist_ok=True)
    cls = np.stack([encode_texts(model, tok, [article_text(t, s) for t in CLASS_TEMPLATES], device).mean(0)
                    for s in prompts.CATEGORIES])
    cls /= np.linalg.norm(cls, axis=1, keepdims=True)
    neg_names, neg_texts = [], []
    for k, v in NEGATIVES.items():
        neg_names += [k] * len(v); neg_texts += v
    neg = encode_texts(model, tok, neg_texts, device)
    gen = encode_texts(model, tok, GENERIC_POS, device)
    np.savez(os.path.join(a.feat, 'text.npz'), categories=np.array(prompts.CATEGORIES), cls=cls,
             neg=neg, neg_names=np.array(neg_names), neg_texts=np.array(neg_texts), generic=gen)
    tars = sorted(f for f in os.listdir(a.raw) if f.endswith('.tar'))
    if a.subjects:                       # only this array task's subjects (same round-robin split as gen.py)
        subs = prompts.read_list(a.subjects) if os.path.exists(a.subjects) else a.subjects.split(',')
        mine = {prompts.slug(x) + '.tar' for x in subs[a.task::a.ntasks]}
        tars = [t for t in tars if t in mine]
    for tf_name in tars:
        slug = tf_name[:-4]
        outp = os.path.join(a.feat, slug + '.npz')
        if os.path.exists(outp) and not a.force:
            continue
        t0 = time.time()
        metas = {}
        for l in open(os.path.join(a.raw, slug + '.jsonl')):
            if l.strip():
                m = json.loads(l); metas[m['seed']] = m
        seeds, i64, i32, thumbs, heur, full, small = [], [], [], [], [], [], []
        with tarfile.open(os.path.join(a.raw, tf_name)) as tf:
            for mem in tf.getmembers():
                seed = int(mem.name.split('.')[0])
                if seed not in metas:
                    continue
                rgb = np.asarray(Image.open(io.BytesIO(tf.extractfile(mem).read())).convert('RGB'))
                a64, a32 = process.process(rgb)
                seeds.append(seed); i64.append(a64); i32.append(a32)
                g = process.to_grey(rgb)
                thumbs.append(np.asarray(Image.fromarray((g * 255).astype(np.uint8)).resize((128, 128), Image.BOX)))
                heur.append(heuristics(rgb))
                full.append(Image.fromarray(rgb))
                small.append(Image.fromarray(process.to_u8(a64)).convert('RGB'))
        ns = nsfw_scores(nsfw, full, device)
        ef = encode_images(model, pre, full, device)
        es = encode_images(model, pre, small, device)
        keys = sorted(heur[0])
        np.savez(outp, seeds=np.array(seeds), img64=np.stack(i64), img32=np.stack(i32),
                 thumb=np.stack(thumbs), emb=ef, emb64=es, nsfw=ns, heur_keys=np.array(keys),
                 heur=np.array([[h[k] for k in keys] for h in heur], np.float32),
                 metas=np.array([json.dumps(metas[s]) for s in seeds]))
        print(f'{slug}: {len(seeds)} images scored in {time.time() - t0:.1f}s', flush=True)


# ---------------------------------------------------------------------------------------- select
def softmax(x):
    x = x - x.max(-1, keepdims=True)
    e = np.exp(x)
    return e / e.sum(-1, keepdims=True)


def select(a):
    th = dict(TH)
    for kv in a.th or []:
        k, v = kv.split('='); th[k] = type(TH[k])(float(v))
    T = np.load(os.path.join(a.feat, 'text.npz'))
    cats = list(T['categories'])
    subj = prompts.SUBJECTS
    cidx = np.array([cats.index(s) for s in subj])
    C = T['cls'][cidx]                                  # zero-shot over the kept subjects
    neg, neg_names = T['neg'], list(T['neg_names'])
    groups = sorted(set(neg_names))
    files = sorted(f for f in os.listdir(a.feat) if f.endswith('.npz') and f != 'text.npz')
    if a.only:                           # e.g. only the subjects whose array tasks have finished
        only = prompts.read_list(a.only) if os.path.exists(a.only) else a.only.split(',')
        files = [f for f in files if f[:-4] in {prompts.slug(x) for x in only}]
    K64, K32, LAB, META, REJ = [], [], [], [], []
    stats = {}
    for fn in files:
        d = np.load(os.path.join(a.feat, fn))
        # NpzFile decompresses the whole array on every d[key] access: read each array exactly once
        img64_all, img32_all, seeds_all = d['img64'], d['img32'], d['seeds']
        metas = [json.loads(m) for m in d['metas']]
        s = metas[0]['subject']; si = subj.index(s)
        H = dict(zip(d['heur_keys'], d['heur'].T))
        H['nsfw'] = d['nsfw']
        emb = d['emb'].astype(np.float32); emb64 = d['emb64'].astype(np.float32)
        lg = 100.0 * emb @ C.T
        rank = (lg > lg[:, [si]]).sum(1) + 1
        ptrue = softmax(lg)[:, si]
        rank64 = ((100.0 * emb64 @ C.T) > (100.0 * emb64 @ C[si])[:, None]).sum(1) + 1
        # negatives: softmax over [generic clip-art positive, all negative texts] (the true class is left
        # out: clip art of the subject matches its own name so strongly that every negative would vanish)
        z = 100.0 * np.concatenate([emb @ T['generic'].T, emb @ neg.T], 1)
        pz = softmax(z)
        pneg = {g: pz[:, 1:][:, [n == g for n in neg_names]].sum(1) for g in groups}
        px = img32_all.reshape(len(emb), -1).astype(np.float32) / 15.0
        n = len(metas)
        frame_rule = float(np.median(H['frame_cov'])) < th['frame_subject_median']
        reasons = [[] for _ in range(n)]
        for j in range(n):
            r = reasons[j]
            if any(x in metas[j]['prompt'] for x in EXCLUDE_PROMPT): r.append('excluded')
            if H['nsfw'][j] > th['nsfw']: r.append('nsfw')
            if pneg['unsafe'][j] > th['p_unsafe']: r.append('unsafe')
            if H['border_white'][j] < th['border_white']: r.append('bg')
            if H['edge_ink'][j] > th['edge_ink']: r.append('edge')
            if H['ink_frac'][j] < th['ink_min']: r.append('faint')
            if H['dark_fill'][j] > th['fill_max']: r.append('heavy')
            if H['colour_frac'][j] > th['colour_frac']: r.append('colour')
            if frame_rule and H['frame_cov'][j] >= th['frame_cov']: r.append('frame')
            if pneg['text'][j] > th['p_text']: r.append('text')
            if pneg['photo'][j] > th['p_photo']: r.append('photo')
            if pneg['dark'][j] > th['p_dark']: r.append('dark')
            if pneg['many'][j] > th['p_many']: r.append('many')
            if pneg['frame'][j] > th['p_frame'] and 'frame' not in r: r.append('frame')
            if H['n_cc5'][j] >= th['multi_n'] or H['second'][j] >= th['multi_second']: r.append('multi')
            if rank[j] > th['rank_max'] or (rank[j] > 1 and ptrue[j] < th['p_min']): r.append('nomatch')
            if rank64[j] > th['rank64_max']: r.append('lost64')
        kept, dup_of = [], {}
        for j in range(n):                                 # near-duplicates among survivors
            if reasons[j]:
                continue
            if kept:
                c, mad = emb[kept] @ emb[j], np.abs(px[kept] - px[j]).mean(1)
                hit = np.flatnonzero((c > th['dup_cos']) & (mad < th['dup_mad']))
                if len(hit):
                    reasons[j].append('dup'); dup_of[j] = int(seeds_all[kept[hit[0]]]); continue
            kept.append(j)
        cnt = {}
        for j in range(n):
            sc = dict(rank=int(rank[j]), p_true=round(float(ptrue[j]), 4), rank64=int(rank64[j]),
                      **{f'p_{g}': round(float(pneg[g][j]), 4) for g in groups},
                      **{k: round(float(H[k][j]), 4) for k in H})
            m = dict(metas[j], source=f"raw/{fn[:-4]}.tar:{metas[j]['seed']}.png", scores=sc)
            if reasons[j]:
                prim = min(reasons[j], key=ORDER.index)
                cnt[prim] = cnt.get(prim, 0) + 1
                REJ.append(dict(m, reasons=reasons[j], primary=prim, **({'dup_of': dup_of[j]} if j in dup_of else {})))
            else:
                META.append(m)
        K64.append(img64_all[kept]); K32.append(img32_all[kept]); LAB += [si] * len(kept)
        stats[s] = dict(generated=n, kept=len(kept), rejected_primary=cnt,
                        rejected_any={c: sum(c in r for r in reasons) for c in ORDER if any(c in r for r in reasons)})
    K64 = np.concatenate(K64) if K64 else np.zeros((0, 64, 64), np.uint8)
    K32 = np.concatenate(K32) if K32 else np.zeros((0, 32, 32), np.uint8)
    LAB = np.array(LAB, np.int16)
    n_all, n_val = len(META), 0
    if a.val_out and a.val_per_subject:  # held-out split: a fixed random sample of kept images per subject
        import random
        by = {}
        for k, l in enumerate(LAB.tolist()):
            by.setdefault(l, []).append(k)
        val = set()
        for si, idx in sorted(by.items()):
            val |= set(random.Random(f'val|{subj[si]}').sample(idx, min(a.val_per_subject, len(idx) // 4)))
        isval = np.array([k in val for k in range(len(LAB))], bool)
        va, tr = np.flatnonzero(isval), np.flatnonzero(~isval)
        process.write_split(a.val_out, K64[va], K32[va], LAB[va], [META[k] for k in va])
        K64, K32, LAB, META = K64[tr], K32[tr], LAB[tr], [META[k] for k in tr]
        n_val = len(va)
        print(f'held out {n_val} images in {a.val_out}')
    process.write_split(a.out, K64, K32, LAB, META)
    with open(os.path.join(a.out, 'rejected.jsonl'), 'w') as f:
        for r in REJ:
            f.write(json.dumps(r) + '\n')
    tot = dict(generated=sum(v['generated'] for v in stats.values()), kept=n_all, train=n_all - n_val, val=n_val)
    tot['reject_rate'] = round(1 - tot['kept'] / max(1, tot['generated']), 4)
    prim = {}
    for v in stats.values():
        for k, c in v['rejected_primary'].items():
            prim[k] = prim.get(k, 0) + c
    tot['rejected_primary'] = dict(sorted(prim.items(), key=lambda x: -x[1]))
    json.dump(dict(total=tot, thresholds=th, per_subject=stats), open(os.path.join(a.out, 'select_stats.json'), 'w'), indent=1)
    print(json.dumps(tot))
    for s, v in stats.items():
        print(f"{s:14s} {v['kept']:3d}/{v['generated']:3d}  {v['rejected_primary']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['score', 'select'])
    ap.add_argument('--raw'); ap.add_argument('--feat', required=True); ap.add_argument('--clip'); ap.add_argument('--nsfw')
    ap.add_argument('--out'); ap.add_argument('--th', nargs='*'); ap.add_argument('--force', action='store_true')
    ap.add_argument('--subjects', help='score: only these subjects (file or comma list), split by --task/--ntasks')
    ap.add_argument('--task', type=int, default=0); ap.add_argument('--ntasks', type=int, default=1)
    ap.add_argument('--only', help='select: only these subjects (file or comma list)')
    ap.add_argument('--val-out', help='select: also write a held-out split here'); ap.add_argument('--val-per-subject', type=int, default=8)
    a = ap.parse_args()
    score(a) if a.cmd == 'score' else select(a)


if __name__ == '__main__':
    main()

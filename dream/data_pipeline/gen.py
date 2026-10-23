"""Generate Dream training images (512x512) and store them on scratch.

Backends: 'klein' = FLUX 3 Image Klein, distilled, via BFL's reference code (klein.py; 16 steps, no
guidance, full captions); 'diffusers' = any diffusers pipeline (FLUX.1-schnell: 4 steps, guidance 0;
FLUX.2 [klein] 4B: 4 steps, guidance 1).

Writes, per subject, <out>/<slug>.tar (PNG files named <seed>.png) and <out>/<slug>.jsonl (one line per
image: subject, i, seed, prompt, style, template, modifier, view, and the short request for upsampled
captions). Resumable: images already listed in the jsonl are skipped. Batches mix subjects.

  python gen.py --backend klein --model M --style caption --subjects pilot_subjects.txt --n 70 --out data/pilot/raw
  python gen.py --backend klein --model M --style pup --captions caps.jsonl --subjects ... --out ...
  python gen.py --backend diffusers --model M --bench 8,16   # throughput only
"""
import argparse
import io
import json
import os
import tarfile
import threading
import queue
import time

import numpy as np
import torch
from PIL import Image

import prompts

# Per-pipeline defaults (model card usage).
GUIDANCE = {'FluxPipeline': 0.0, 'Flux2KleinPipeline': 1.0}
CFG = dict(size=512, steps=None, guidance=None, max_seq=256)


class Diffusers:
    def __init__(self, model):
        from diffusers import DiffusionPipeline      # class chosen by the model's model_index.json
        self.pipe = DiffusionPipeline.from_pretrained(model, torch_dtype=torch.bfloat16).to('cuda')
        self.pipe.set_progress_bar_config(disable=True)
        self.name = type(self.pipe).__name__
        if CFG['guidance'] is None:
            CFG['guidance'] = GUIDANCE.get(self.name, 0.0)
        if CFG['steps'] is None:
            CFG['steps'] = 4

    def __call__(self, texts, seeds):
        gens = [torch.Generator('cuda').manual_seed(int(s)) for s in seeds]
        with torch.inference_mode():
            out = self.pipe(prompt=list(texts), height=CFG['size'], width=CFG['size'],
                            num_inference_steps=CFG['steps'], guidance_scale=CFG['guidance'],
                            max_sequence_length=CFG['max_seq'], generator=gens, output_type='np')
        return (np.clip(np.asarray(out.images, dtype=np.float32), 0, 1) * 255 + 0.5).astype(np.uint8)


class KleinBackend:
    def __init__(self, model):
        from klein import Klein
        self.k = Klein(model)
        self.name = f'{Klein.name}, attention {self.k.attention}'
        if CFG['steps'] is None:
            CFG['steps'] = 16
        CFG['guidance'] = None

    def __call__(self, texts, seeds):
        return self.k(list(texts), seeds, size=CFG['size'], steps=CFG['steps'])


def bench(run, sizes, style):
    res = {}
    texts = [prompts.make_prompt('cat', i, style='short' if style == 'pup' else style)[0] for i in range(64)]
    for b in sizes:
        run(texts[:b], range(b))                              # warm-up at this shape
        torch.cuda.synchronize(); t = time.time(); n = 0
        for k in range(2):
            run(texts[:b], range(1000 + k * b, 1000 + (k + 1) * b)); n += b
        torch.cuda.synchronize(); dt = time.time() - t
        res[b] = n / dt
        print(f'bench batch {b}: {n / dt:.3f} images/s  (peak mem {torch.cuda.max_memory_allocated() / 2**30:.1f} GiB)', flush=True)
    return res


def writer(q):
    """Encode PNGs and append them to per-subject tars (handles kept open; the jsonl line is written
    only after the tar member is flushed, so a listed image is always in the tar)."""
    open_tars, open_jsonl = {}, {}
    while True:
        item = q.get()
        if item is None:
            break
        path_tar, path_jsonl, img, meta = item
        buf = io.BytesIO()
        Image.fromarray(img).save(buf, format='PNG', compress_level=6)
        data = buf.getvalue()
        if path_tar not in open_tars:
            open_tars[path_tar] = tarfile.open(path_tar, 'a')
            open_jsonl[path_tar] = open(path_jsonl, 'a')
        tf = open_tars[path_tar]
        ti = tarfile.TarInfo(f"{meta['seed']}.png"); ti.size = len(data); ti.mtime = int(time.time())
        tf.addfile(ti, io.BytesIO(data)); tf.fileobj.flush()
        open_jsonl[path_tar].write(json.dumps(meta) + '\n'); open_jsonl[path_tar].flush()
    for tf in open_tars.values():
        tf.close()
    for f in open_jsonl.values():
        f.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--backend', choices=['klein', 'diffusers'], default='klein')
    ap.add_argument('--model', required=True)
    ap.add_argument('--subjects', help='file with one subject per line, or comma list')
    ap.add_argument('--task', type=int, default=0, help='array task id (subjects split round-robin)')
    ap.add_argument('--ntasks', type=int, default=1)
    ap.add_argument('--n', type=int, default=70, help='images per subject')
    ap.add_argument('--start', type=int, default=0, help='first image index i')
    ap.add_argument('--out')
    ap.add_argument('--batch', type=int, default=16)
    ap.add_argument('--bench', default='')
    ap.add_argument('--style', choices=['short', 'caption', 'caption_bbox', 'pup'], default='caption')
    ap.add_argument('--captions', help='upsampled captions jsonl (style pup), from upsample.py')
    ap.add_argument('--template', type=int, help='fixed template, no modifier/view (comparisons)')
    ap.add_argument('--steps', type=int)
    ap.add_argument('--guidance', type=float, help='diffusers only; default per pipeline (GUIDANCE)')
    ap.add_argument('--size', type=int, default=512)
    ap.add_argument('--max-seq', type=int, default=256)
    a = ap.parse_args()
    CFG.update(size=a.size, steps=a.steps, guidance=a.guidance, max_seq=a.max_seq)

    t0 = time.time()
    run = KleinBackend(a.model) if a.backend == 'klein' else Diffusers(a.model)
    print(f'{run.name} loaded in {time.time() - t0:.0f}s; {CFG}', flush=True)
    if a.bench:
        r = bench(run, [int(x) for x in a.bench.split(',')], a.style)
        if a.out:
            os.makedirs(a.out, exist_ok=True)
            json.dump({'bench_images_per_s': r, 'gpu': torch.cuda.get_device_name(), 'backend': run.name,
                       'style': a.style, 'cfg': CFG}, open(os.path.join(a.out, 'bench.json'), 'w'), indent=1)
        if not a.subjects:
            return

    caps = {}
    if a.style == 'pup':
        for l in open(a.captions):
            if l.strip():
                c = json.loads(l); caps[(c['subject'], c['i'])] = c
    subs = prompts.read_list(a.subjects) if os.path.exists(a.subjects) else a.subjects.split(',')
    subs = subs[a.task::a.ntasks]
    os.makedirs(a.out, exist_ok=True)
    jobs = []
    for s in subs:
        jl = os.path.join(a.out, prompts.slug(s) + '.jsonl')
        done = set()
        if os.path.exists(jl):
            done = {json.loads(l)['seed'] for l in open(jl) if l.strip()}
        for i in range(a.start, a.start + a.n):
            seed = prompts.seed_for(s, i)
            if seed in done:
                continue
            if a.style == 'pup':
                c = caps[(s, i)]
                jobs.append(dict(subject=s, i=i, seed=seed, prompt=c['caption'], request=c['request'], style='pup'))
            else:
                text, t, m, v = prompts.make_prompt(s, i, a.template, style=a.style)
                jobs.append(dict(subject=s, i=i, seed=seed, prompt=text, style=a.style, template=t, modifier=m, view=v))
    print(f'{len(subs)} subjects, {len(jobs)} images to generate', flush=True)

    q = queue.Queue(maxsize=256)
    th = threading.Thread(target=writer, args=(q,), daemon=True); th.start()
    times, counts = [], []
    for k in range(0, len(jobs), a.batch):
        chunk = jobs[k:k + a.batch]
        torch.cuda.synchronize(); t = time.time()
        imgs = run([j['prompt'] for j in chunk], [j['seed'] for j in chunk])
        torch.cuda.synchronize(); dt = time.time() - t
        if k > 0 and len(chunk) == a.batch:      # skip the warm-up batch and the ragged tail
            times.append(dt); counts.append(len(chunk))
        for img, j in zip(imgs, chunk):
            slug = prompts.slug(j['subject'])
            q.put((os.path.join(a.out, slug + '.tar'), os.path.join(a.out, slug + '.jsonl'), img, j))
        if (k // a.batch) % 10 == 0:
            rate = sum(counts) / sum(times) if times else float('nan')
            print(f'[{time.strftime("%H:%M:%S")}] {k + len(chunk)}/{len(jobs)}  {rate:.3f} images/s', flush=True)
    q.put(None); th.join()
    rate = sum(counts) / sum(times) if times else float('nan')
    stats = dict(task=a.task, images=len(jobs), batch=a.batch, images_per_s=rate, backend=run.name, style=a.style,
                 cfg=CFG, model=a.model, gpu=torch.cuda.get_device_name(), wall_s=time.time() - t0)
    print(json.dumps(stats), flush=True)
    with open(os.path.join(a.out, f'gen_stats_{a.task:03d}.json'), 'w') as f:
        json.dump(stats, f, indent=1)


if __name__ == '__main__':
    main()

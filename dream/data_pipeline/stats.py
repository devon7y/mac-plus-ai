"""Summarize a data run into stats.json: per-subject generated/kept counts and rejection reasons
(from filter.py select), generation speed (gen_stats_*.json, bench.json), GPU-hours used, and the
GPU-hour estimate for a full run.

  python stats.py --split data/pilot/split --raw data/pilot/raw --bench data/bench_*_8steps \
      --gpu-hours-used 0.4 --out stats.json [--m2-subjects 261 --m2-per-subject 600]
"""
import argparse
import glob
import json
import os

import numpy as np

import prompts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--split', required=True)
    ap.add_argument('--raw', required=True)
    ap.add_argument('--bench', nargs='*', default=[])
    ap.add_argument('--gpu-hours-used', default=None,
                    help='from sacct: a number, or key=hours pairs, e.g. fir_mig3g40=16.7,nibi_h100=2.6')
    ap.add_argument('--mig-rate', type=float, help='measured images/s on one 3g.40gb MIG slice')
    ap.add_argument('--score-rate', type=float, help='measured scoring images/s (filter.py score) on that slice')
    ap.add_argument('--m2-subjects', type=int, default=len(prompts.SUBJECTS))
    ap.add_argument('--m2-per-subject', type=int, default=600)
    ap.add_argument('--out', required=True)
    ap.add_argument('--npz', help='also write kept images (64, 32, labels) to this compressed npz')
    a = ap.parse_args()
    sel = json.load(open(os.path.join(a.split, 'select_stats.json')))
    gens = [json.load(open(f)) for f in sorted(glob.glob(os.path.join(a.raw, 'gen_stats_*.json')))]
    gh = a.gpu_hours_used
    if gh and '=' in gh:
        gh = {k: float(v) for k, v in (p.split('=') for p in gh.split(','))}
    elif gh:
        gh = float(gh)
    bench = {}
    for d in a.bench:
        for f in glob.glob(os.path.join(d, 'bench.json')):
            b = json.load(open(f))
            bench[f"{b['gpu']} | {b['cfg']['steps']} steps"] = b['bench_images_per_s']
    full = [max(v.values()) for k, v in bench.items() if 'MIG' not in k and k.endswith(f"| {gens[0]['cfg']['steps']} steps")] if gens else []
    rate = max(full) if full else None
    keep = sel['total']['kept'] / max(1, sel['total']['generated'])
    n_m2 = a.m2_subjects * a.m2_per_subject
    out = dict(
        generator=gens[0]['backend'] if gens else None,
        settings=gens[0]['cfg'] if gens else None,
        style=gens[0].get('style') if gens else None,
        total=sel['total'],
        thresholds=sel['thresholds'],
        per_subject={s: dict(generated=v['generated'], kept=v['kept'], rejected=v['rejected_primary'])
                     for s, v in sel['per_subject'].items()},
        generation=[dict(task=g.get('task'), gpu=g['gpu'], images=g['images'], batch=g['batch'],
                         images_per_s=round(g['images_per_s'], 3), wall_s=round(g['wall_s'])) for g in gens],
        bench_images_per_s=bench,
        gpu_hours_used=gh,
        m2_estimate=dict(subjects=a.m2_subjects, images_per_subject=a.m2_per_subject, images=n_m2,
                         expected_kept=int(n_m2 * keep),
                         full_h100_images_per_s=rate,
                         generation_h100_hours=round(n_m2 / rate / 3600, 1) if rate else None,
                         mig_3g40_images_per_s=a.mig_rate,
                         generation_mig_3g40_hours=round(n_m2 / a.mig_rate / 3600, 1) if a.mig_rate else None,
                         scoring_images_per_s_mig=a.score_rate,
                         scoring_mig_3g40_hours=round(n_m2 / a.score_rate / 3600, 1) if a.score_rate else None,
                         note='a 3g.40gb MIG slice is 3/7 of an H100 but reaches 55% of its throughput (the full GPU '
                              'is underused by per-sample loops in the reference code); add ~2 min per array task '
                              'for the venv build and model copy'),
    )
    json.dump(out, open(a.out, 'w'), indent=1)
    print(json.dumps({k: out[k] for k in ('total', 'bench_images_per_s', 'm2_estimate')}, indent=1))
    if a.npz:
        np.savez_compressed(a.npz, images64=np.load(os.path.join(a.split, 'images64.npy')),
                            images32=np.load(os.path.join(a.split, 'images32.npy')),
                            labels=np.load(os.path.join(a.split, 'labels.npy')),
                            subjects=np.array(prompts.SUBJECTS))


if __name__ == '__main__':
    main()

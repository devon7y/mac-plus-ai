"""Expand short requests into full captions with BFL's prompt upsampler for FLUX 3 Image Klein
(black-forest-labs/flux-3-image-klein-pup, Qwen3.5 9B, Apache-2.0), as on its model card:
system_prompts.json["t2i"] as the system prompt, greedy decoding, thinking off.

  python upsample.py --pup models/flux-3-image-klein-pup --subjects pilot_subjects.txt --n 2 --out caps.jsonl

Each output line: subject, i, request (the short prompt from prompts.py), caption.
"""
import argparse
import json
import os
import time

import torch
from transformers import AutoProcessor, Qwen3_5ForConditionalGeneration

import prompts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pup', required=True)
    ap.add_argument('--subjects', required=True)
    ap.add_argument('--n', type=int, default=2)
    ap.add_argument('--start', type=int, default=0)
    ap.add_argument('--out', required=True)
    ap.add_argument('--batch', type=int, default=20)
    ap.add_argument('--max-new', type=int, default=1024)
    a = ap.parse_args()
    t0 = time.time()
    proc = AutoProcessor.from_pretrained(a.pup)
    tok = proc.tokenizer
    tok.padding_side = 'left'
    model = Qwen3_5ForConditionalGeneration.from_pretrained(a.pup, dtype=torch.bfloat16, device_map='cuda').eval()
    system = json.load(open(os.path.join(a.pup, 'system_prompts.json')))['t2i']
    print(f'upsampler loaded in {time.time() - t0:.0f}s', flush=True)
    subs = prompts.read_list(a.subjects) if os.path.exists(a.subjects) else a.subjects.split(',')
    jobs = [(s, i, prompts.make_prompt(s, i, style='short')[0]) for s in subs for i in range(a.start, a.start + a.n)]
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    ntok, t1 = 0, time.time()
    with open(a.out, 'w') as f:
        for k in range(0, len(jobs), a.batch):
            chunk = jobs[k:k + a.batch]
            texts = [proc.apply_chat_template([{'role': 'system', 'content': system}, {'role': 'user', 'content': req}],
                                              add_generation_prompt=True, tokenize=False, enable_thinking=False)
                     for _, _, req in chunk]
            inputs = tok(texts, return_tensors='pt', padding=True).to('cuda')
            with torch.no_grad():
                out = model.generate(**inputs, max_new_tokens=a.max_new, do_sample=False)
            L = inputs['input_ids'].shape[1]
            for (s, i, req), row in zip(chunk, out):
                cap = tok.decode(row[L:], skip_special_tokens=True).strip()
                ntok += int((row[L:] != tok.pad_token_id).sum())
                f.write(json.dumps(dict(subject=s, i=i, request=req, caption=cap)) + '\n')
            print(f'[{time.strftime("%H:%M:%S")}] {k + len(chunk)}/{len(jobs)} captions, {ntok / (time.time() - t1):.0f} tok/s', flush=True)
    print(f'done: {len(jobs)} captions, {ntok} tokens in {time.time() - t1:.0f}s', flush=True)


if __name__ == '__main__':
    main()

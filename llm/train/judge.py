#!/usr/bin/env python3
"""LLM-judge rubric for Pixel replies (vLLM, Qwen2.5-7B-Instruct, temperature 0).

Input: val_replies_<name>.jsonl files from sample_chats.py (each Pixel turn has the model reply and
the reference reply from the corpus). Every Pixel turn is graded on three 1-5 scales given the chat so
far: grammar, coherence (makes sense as an answer to the user), persona (consistent with Pixel).
The corpus references are graded the same way as a ceiling (--with-ref).

Usage: python judge.py --model <qwen dir> --out judge.json val_replies_ternary.jsonl val_replies_fp.jsonl
"""
import argparse
import json
import re
import statistics
import sys

SYSTEM = """You grade replies written by Pixel, a toy chatbot. Pixel is a cheerful little Macintosh
computer from 1986 that lives on a desk, loves floppy disks, says "Beep!" when happy, thinks the
Chicago font is beautiful, is scared of magnets, likes computer mice, and is a bit confused about what
year it is. Pixel replies in 1 or 2 short, simple sentences, cannot eat or go outside, and does not
know about news, phones or the internet.

Grade ONLY the last Pixel reply, given the chat before it, on three scales from 1 (bad) to 5 (good):
- grammar: fluent, grammatical, complete English sentences.
- coherence: makes sense and fits what the user just said.
- persona: consistent with Pixel's facts and style above.
Answer with JSON only, like {"grammar": 4, "coherence": 3, "persona": 5}."""


def prompts_for(rec, which):
    out = []
    hist = []
    for t in rec["turns"]:
        hist.append(f"User: {t['user']}")
        reply = t[which]
        out.append("\n".join(hist + [f"Pixel: {reply}"]))
        hist.append(f"Pixel: {t['pixel']}")  # the model's own reply is what it remembered
    return out


def parse(text):
    m = re.search(r"\{.*?\}", text, re.S)
    try:
        d = json.loads(m.group(0))
        return {k: max(1, min(5, int(d[k]))) for k in ("grammar", "coherence", "persona")}
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--with-ref", action="store_true")
    ap.add_argument("inputs", nargs="+")
    args = ap.parse_args()
    from vllm import LLM, SamplingParams
    llm = LLM(model=args.model, dtype="bfloat16", max_model_len=4096, gpu_memory_utilization=0.9,
              enable_prefix_caching=True)
    sp = SamplingParams(temperature=0.0, max_tokens=40)
    results = {}
    jobs = []
    for path in args.inputs:
        name = re.sub(r"^.*val_replies_|\.jsonl$", "", path)
        recs = [json.loads(l) for l in open(path)]
        jobs.append((name, recs, "pixel"))
        if args.with_ref and "ref" not in results:
            jobs.append(("corpus_reference", recs, "ref"))
            results["ref"] = True
    results = {}
    for name, recs, which in jobs:
        convs = [p for r in recs for p in prompts_for(r, which)]
        msgs = [[{"role": "system", "content": SYSTEM},
                 {"role": "user", "content": "Chat:\n" + c + "\n\nGrade the last Pixel reply."}] for c in convs]
        outs = llm.chat(msgs, sp, use_tqdm=False)
        scores = [parse(o.outputs[0].text) for o in outs]
        ok = [s for s in scores if s]
        summ = {k: round(statistics.mean(s[k] for s in ok), 3) for k in ("grammar", "coherence", "persona")}
        summ["overall"] = round(statistics.mean(summ.values()), 3)
        summ["n_graded"] = len(ok)
        summ["n_unparsed"] = len(scores) - len(ok)
        results[name] = summ
        print(name, json.dumps(summ), flush=True)
    json.dump(results, open(args.out, "w"), indent=1)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Compare the training model with the integer reference (llm/ref) and export the model file.

1. logic check: torch QAT model (float64, full band-masked forward and the KV-cache step) vs
   ref FloatModel(qat=True) fed the *effective* weights torch uses (ternary * alpha, router and
   embedding fake-quantized). Should agree to float precision.
2. deployment check: latent weights -> ref export.export (B5 packing, SF scales) -> IntModel (bit-exact
   Mac arithmetic), teacher-forced on token sequences; logit correlation and top-1 agreement vs the
   torch QAT model.
3. optional: write the Pixel model file (export.write_model).

Usage: python3 check_ref.py [--ckpt best.pt] [--tokens tokens.txt|--random] [--out model.pxl]
"""
import argparse
import json
import os
import sys

import numpy as np
import torch

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.environ.get("PIXEL_REF", os.path.join(HERE, "..", "ref"))
sys.path.insert(0, REF)          # ref first: its export.py does `from model import ...`
sys.path.append(HERE)
import model as RM  # noqa: E402  (ref integer/float model)
import export as EX  # noqa: E402
import sf  # noqa: E402
import importlib.util  # noqa: E402

_spec = importlib.util.spec_from_file_location("pixel_train_model", os.path.join(HERE, "model.py"))
TM = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(TM)


def effective_weights(m):
    """Weights exactly as the torch QAT forward uses them, in export's float layout."""
    sd = {"tok_emb": m.table(), "final_norm": m.final_norm}
    for l, L in enumerate(m.layers):
        for k in ("attn_norm", "ffn_norm"):
            sd[f"layers.{l}.{k}"] = getattr(L, k)
        for k in ("wq", "wk", "wv", "wo", "router", "w_gate", "w_up", "w_down"):
            sd[f"layers.{l}.{k}"] = L.W(k)
    return TM.to_float_weights(sd)


def corr_top1(a, b):
    cs = [np.corrcoef(x, y)[0, 1] for x, y in zip(a, b)]
    t1 = np.mean([np.argmax(x) == np.argmax(y) for x, y in zip(a, b)])
    return float(np.mean(cs)), float(np.min(cs)), float(t1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt")
    ap.add_argument("--tok", default=os.path.join(HERE, "tokenizer.json"))
    ap.add_argument("--prompts", nargs="*", default=[
        "<user> hi pixel!<pixel> Beep! Hello, friend! How are you today?<end><user> i am good. what are you?<pixel>",
        "<user> what year is it<pixel> I think it is 1986. Is it?<end><user> no its 2026 lol<pixel>",
        "<user> do you like magnets?<pixel>",
        "<user> I had pizza for lunch!<pixel> Yum! What was on it?<end><user> cheese and more cheese<pixel> Beep! That sounds so good.<end><user> what is your favorite color?<pixel>",
    ])
    ap.add_argument("--random", action="store_true")
    ap.add_argument("--fp", action="store_true", help="random full-precision model (logic check)")
    ap.add_argument("--n-int", type=int, default=2, help="sequences to run through IntModel (slow)")
    ap.add_argument("--out")
    ap.add_argument("--report")
    args = ap.parse_args()

    torch.set_default_dtype(torch.float64)
    if args.ckpt:
        ck = torch.load(args.ckpt, map_location="cpu", weights_only=False)
        cfg = TM.Config(**ck["cfg"])
        m = TM.build(cfg)
        m.load_state_dict({k.replace("_orig_mod.", ""): v for k, v in ck["model"].items()})
    else:
        cfg = TM.Config(quant=not args.fp)
        torch.manual_seed(0)
        m = TM.build(cfg)
    m = m.double().eval()
    if os.path.exists(args.tok) and not args.random:
        import tokenizer as T
        tok = T.Tokenizer.load(args.tok)
        seqs = [tok.encode(p) for p in args.prompts]
    else:
        rng = np.random.default_rng(0)
        seqs = [list(rng.integers(4, 2048, 90)) for _ in range(3)]
    # make sequences exceed the window by repeating/concatenating
    long = sum(seqs, [])
    seqs = [long[:100]] + seqs
    rep = {"seq_lens": [len(s) for s in seqs]}

    with torch.no_grad():
        # 1a. full forward vs step
        fulls, steps = [], []
        for s in seqs:
            x = torch.tensor([s])
            fulls.append(m(x)[0][0].numpy())
            cache = m.new_cache()
            steps.append(np.stack([m.step(torch.tensor([t]), cache)[0].numpy() for t in s]))
        rep["full_vs_step_maxabs"] = float(max(np.abs(a - b).max() for a, b in zip(fulls, steps)))
        # 1b. vs ref FloatModel(qat) on the effective weights
        fw_eff = effective_weights(m)
        refs = []
        for s in seqs:
            fm = RM.FloatModel(fw_eff, qat=cfg.quant)
            refs.append(np.stack([fm.step(t) for t in s]))
        rep["torch_vs_refFloat_maxabs"] = float(max(np.abs(a - b).max() for a, b in zip(fulls, refs)))
        rep["torch_vs_refFloat_corr_top1"] = corr_top1(np.concatenate(fulls), np.concatenate(refs))

    # 2. export latent weights and run the integer model
    fw = TM.to_float_weights(m.state_dict())
    pm = EX.export(fw)
    ints, flo = [], []
    for s in seqs[:args.n_int]:
        im = RM.IntModel(pm)
        for i, t in enumerate(s):
            Kl, sigma = im.step(int(t))
            ints.append(Kl.astype(np.float64) * sf.value(sigma))
    flo = np.concatenate(fulls[:args.n_int])
    rep["int_vs_torchQAT_corr_mean_min_top1"] = corr_top1(np.stack(ints), flo)
    # also vs ref float QAT on the dequantized packed weights
    fdq = EX.dequantize(pm)
    dq = []
    for s in seqs[:args.n_int]:
        fm = RM.FloatModel(fdq, qat=True)
        dq.extend(fm.step(t) for t in s)
    rep["int_vs_refQATdequant_corr_mean_min_top1"] = corr_top1(np.stack(ints), np.stack(dq))
    rep["torchQAT_vs_refQATdequant_corr_mean_min_top1"] = corr_top1(flo, np.stack(dq))
    if args.out:
        pm["merges"] = EX.tokenizer_merges(EX.load_tokenizer(args.tok))
        EX.write_model(pm, args.out)
        rep["model_file"] = args.out
        rep["model_bytes"] = os.path.getsize(args.out)
    print(json.dumps(rep, indent=1))
    if args.report:
        json.dump(rep, open(args.report, "w"), indent=1)


if __name__ == "__main__":
    main()

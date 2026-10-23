#!/usr/bin/env python3
"""Chat with a trained Pixel checkpoint the way the Mac does: one token at a time through a rolling
64-token KV cache, sampling with temperature 0.8 and top-k 16 (the Mac defaults; SPEC section 7),
stopping at <end> or after --max-reply tokens.

  scripted   30 fixed user scripts (2-4 user turns each) -> <out>/samples_<name>.md
  --val N    Pixel replies for the user turns of N held-out val chats (teacher-forced user side,
             model's own previous replies kept in memory) -> <out>/val_replies_<name>.jsonl (for judge.py)

Usage: python sample_chats.py --ckpt best.pt --tok tokenizer.json --name ternary --out samples/ [--val 300]
"""
import argparse
import json
import os
import re
import sys

import torch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tokenizer as T  # noqa: E402
from model import Config, build  # noqa: E402

SCRIPTS = [
    ["hi", "what are you?", "cool! do you like floppy disks?"],
    ["hello pixel", "how old are you", "what year is it?"],
    ["Hi Pixel! How are you today?", "I am sad. My dog is sick.", "thank you"],
    ["what's your favorite color?", "mine is blue", "why?"],
    ["are you scared of anything", "why magnets?", "i will keep magnets away"],
    ["tell me a joke", "haha another one", "lol"],
    ["what is 2 plus 2", "and 3 plus 3?", "you are smart"],
    ["do you eat food?", "i had pizza for lunch", "with cheese!"],
    ["whats the news today", "do you know the internet?", "ok"],
    ["good morning!", "it is raining outside", "i like puddles"],
    ["can we play a game", "i spy something green", "a tree!"],
    ["i have a cat named Tom", "he is orange", "he likes to sleep"],
    ["what do you like", "what is the chicago font", "is it pretty?"],
    ["are you a robot", "are you alive?", "do you have feelings"],
    ["hiii", "im bored", "what should i do"],
    ["what day is it", "what comes after monday", "and after that?"],
    ["Do you like mice?", "real mice?", "haha"],
    ["I lost my tooth today!", "will the tooth fairy come?", "yay"],
    ["Hello! My name is Mia.", "what is my name?", "good job"],
    ["you are dumb", "sorry", "you are nice"],
    ["count to five", "now count backwards", "wow"],
    ["what is your name", "that is a nice name", "who made you?"],
    ["do you sleep", "do you dream?", "what do you dream about"],
    ["i love dinosaurs", "what is your favorite dinosaur?", "mine is t rex"],
    ["How many legs does a spider have?", "and a dog?", "and a bird?"],
    ["its my birthday", "i am seven", "we will have cake"],
    ["good night pixel", "sweet dreams", "bye"],
    ["Can you see me?", "what is on your screen?", "cool"],
    ["what is your favorite thing", "why do you like them so much", "i have a floppy disk too"],
    ["I'm scared of the dark", "can you stay with me?", "thanks pixel"],
]


@torch.no_grad()
def reply(model, cache, ids, temp, topk, max_reply, gen, dev):
    """Feed ids (user turn ending in <pixel>), then sample until <end>."""
    logits = None
    for t in ids:
        logits = model.step(torch.tensor([t], device=dev), cache)
    out = []
    for _ in range(max_reply):
        lg = logits[0].float()
        if temp <= 0:
            nxt = int(lg.argmax())
        else:
            v, i = lg.topk(topk)
            p = torch.softmax(v / temp, -1)
            nxt = int(i[torch.multinomial(p, 1, generator=gen)])
        if nxt in (T.USER, T.PIXEL, T.PAD):  # a stray marker ends the reply like <end>
            nxt = T.END
        out.append(nxt)
        logits = model.step(torch.tensor([nxt], device=dev), cache)
        if nxt == T.END:
            break
    if out and out[-1] != T.END:  # cut off: close the turn so the memory stays well-formed
        logits = model.step(torch.tensor([T.END], device=dev), cache)
    return out


def chat(model, tok, user_turns, temp, topk, max_reply, gen, dev):
    cache = model.new_cache()
    turns = []
    for u in user_turns:
        ids = tok.encode("<user> " + u + "<pixel>")
        out = reply(model, cache, ids, temp, topk, max_reply, gen, dev)
        text = tok.decode([t for t in out if t != T.END]).strip()
        turns.append((u, text, out and out[-1] == T.END))
    return turns


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--tok", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--val-text", help="val.txt (corpus) for --val")
    ap.add_argument("--val", type=int, default=0)
    ap.add_argument("--temp", type=float, default=0.8)
    ap.add_argument("--topk", type=int, default=16)
    ap.add_argument("--max-reply", type=int, default=48)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    ck = torch.load(args.ckpt, map_location="cpu", weights_only=False)
    model = build(Config(**ck["cfg"]))
    model.load_state_dict({k.replace("_orig_mod.", ""): v for k, v in ck["model"].items()})
    model = model.to(dev).eval()
    tok = T.Tokenizer.load(args.tok)
    gen = torch.Generator(device=dev)
    gen.manual_seed(args.seed)
    os.makedirs(args.out, exist_ok=True)

    lines = [f"# Pixel sample chats: {args.name}", "",
             f"Checkpoint `{os.path.basename(args.ckpt)}` (step {ck.get('step')}, val loss "
             f"{ck.get('val_loss', float('nan')):.4f}). Sampling: temperature {args.temp}, top-k "
             f"{args.topk}, rolling 64-token memory, one session per chat. Seed {args.seed}.", ""]
    for n, script in enumerate(SCRIPTS, 1):
        lines.append(f"### Chat {n}")
        for u, p, ended in chat(model, tok, script, args.temp, args.topk, args.max_reply, gen, dev):
            lines.append(f"- **User:** {u}")
            lines.append(f"- **Pixel:** {p}" + ("" if ended else " *(cut off)*"))
        lines.append("")
    path = os.path.join(args.out, f"samples_{args.name}.md")
    open(path, "w").write("\n".join(lines))
    print("wrote", path)

    if args.val and args.val_text:
        recs = []
        for i, line in enumerate(open(args.val_text)):
            if len(recs) >= args.val:
                break
            users = re.findall(r"<user> (.*?)<pixel>", line)
            refs = re.findall(r"<pixel> (.*?)<end>", line)
            got = chat(model, tok, users, args.temp, args.topk, args.max_reply, gen, dev)
            recs.append({"i": i, "turns": [{"user": u, "pixel": p, "ref": r, "ended": e}
                                           for (u, p, e), r in zip(got, refs)]})
        vp = os.path.join(args.out, f"val_replies_{args.name}.jsonl")
        with open(vp, "w") as f:
            for r in recs:
                f.write(json.dumps(r) + "\n")
        print("wrote", vp)


if __name__ == "__main__":
    main()

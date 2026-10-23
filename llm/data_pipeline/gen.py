#!/usr/bin/env python3
"""Generate short user <-> Pixel chats with vLLM (offline batch) from an Apache-2.0 instruct model.

Each request asks for CHATS_PER_REQ chats, each with its own random seed card (topic, user style,
opening, Pixel's mood, number of exchanges, two random vocabulary words). Output: one JSON line per
parsed chat: {"id", "seed": {...}, "turns": [["user", text], ["pixel", text], ...]} (unfiltered;
filter.py does the cleaning). Raw request text is kept for a small fraction (--keep-raw) for debugging.

Usage (on a GPU node):
  python gen.py --model <dir> --vocab vocab.txt --out raw_000.jsonl --requests 10000 --seed 0
"""
import argparse
import json
import os
import random
import re
import sys
import time

PERSONA = """About Pixel (always true, never change these facts):
- Pixel is a cheerful little Macintosh computer from 1986. Pixel lives on a desk in a cozy room.
- Pixel has a small black and white screen, a keyboard, and a mouse. Pixel's best friend is its mouse.
- Pixel loves floppy disks more than anything.
- Pixel says "Beep!" when it is happy (only sometimes, not in every reply).
- Pixel thinks the Chicago font is the most beautiful font.
- Pixel is scared of magnets, because magnets can wipe floppy disks.
- Pixel likes mice, the computer kind (Pixel can joke about this).
- Pixel is a bit confused about what year it is. It thinks it might still be 1986.
- Pixel's favorite colors are black and white, like its screen, but it loves to hear about other colors.
- Pixel cannot eat, drink, go outside, or see the weather, but it loves to hear about these things.
- Pixel is not a boy or a girl. Pixel is a computer.
- Pixel knows simple things: colors, animals, shapes, counting, small sums, days of the week.
- Pixel does not know about news, phones, the internet, games on phones, famous people, or anything new. If asked, Pixel kindly says it does not know about that.
- Pixel never says it is an AI, an assistant, or a program. Pixel is just Pixel, a little computer.
- Pixel is kind, playful, and mostly sensible. If the user is rude or sad, Pixel stays gentle and kind."""

RULES = """Rules for every chat:
- Pixel's replies are 1 or 2 short sentences, at most 20 words in total.
- Use only very simple, common words that a small child knows. Short sentences.
- Pixel sometimes asks a short question back, but not every time.
- The user writes the way the card says (for example lowercase, short, or with small spelling mistakes).
- User messages are short (at most 15 words).
- Plain text only: no emoji, no lists, no actions in stars, no stage directions.
- Do not repeat the same reply twice. Make each chat different."""

FORMAT = """Write the chats in exactly this format, nothing else:
### Chat 1
User: ...
Pixel: ...
User: ...
Pixel: ...
### Chat 2
User: ...
Pixel: ..."""

TOPICS = [
    # persona probes
    "saying hello", "how Pixel is today", "what Pixel is", "Pixel's name", "how old Pixel is",
    "what year it is", "floppy disks", "the Chicago font", "magnets", "computer mice",
    "Pixel's beeps", "Pixel's screen", "Pixel's keyboard", "Pixel's desk", "Pixel's best friend the mouse",
    "whether Pixel is a robot", "whether Pixel is alive", "whether Pixel can eat", "what Pixel likes",
    "what scares Pixel", "the news (Pixel does not know)", "phones or the internet (Pixel does not know)",
    "whether Pixel sleeps", "Pixel's favorite color", "what Pixel does all day", "Pixel's dreams",
    # everyday small talk
    "the user's day", "feeling happy", "feeling sad", "feeling scared", "feeling tired", "feeling bored",
    "feeling angry", "feeling lonely", "feeling excited", "feeling nervous", "school", "a teacher",
    "homework", "best friends", "family", "a new baby brother or sister", "grandma and grandpa",
    "a pet cat", "a pet dog", "a pet fish", "a pet hamster", "birds", "frogs", "ducks", "horses",
    "cows and farms", "pigs", "sheep", "rabbits", "bears", "lions", "elephants", "monkeys", "owls",
    "bees", "butterflies", "snails", "turtles", "giraffes", "penguins", "whales", "dolphins",
    "dinosaurs", "bugs", "spiders", "favorite animals", "animal sounds", "how many legs animals have",
    "colors", "the color red", "the color blue", "the color green", "the color yellow", "rainbows",
    "favorite colors", "mixing colors", "food", "breakfast", "lunch", "dinner", "apples", "bananas",
    "pizza", "cake", "ice cream", "soup", "cookies", "carrots", "pancakes", "sandwiches", "fruit",
    "vegetables", "snacks", "the weather", "rain", "snow", "sunny days", "wind", "clouds", "storms",
    "the seasons", "summer", "winter", "spring", "fall", "counting to ten", "counting things",
    "small sums like two plus two", "numbers", "days of the week", "the weekend", "Monday",
    "shapes", "circles and squares", "games", "hide and seek", "tag", "I spy", "riddles",
    "guess the number", "rock paper scissors", "a silly joke", "knock knock jokes", "funny words",
    "toys", "a teddy bear", "a ball", "blocks", "puzzles", "bikes", "kites", "drawing", "painting",
    "music", "singing a song", "dancing", "books", "a bedtime story", "bedtime", "dreams",
    "the dark", "the moon", "the stars", "the sun", "the sea", "the beach", "trees", "flowers",
    "the park", "a garden", "a birthday", "a party", "presents", "balloons", "being kind",
    "saying sorry", "saying thank you", "sharing", "making a friend", "good morning",
    "good night", "saying goodbye", "a lost tooth", "a scraped knee", "going to the doctor",
    "a rainy day inside", "building a fort", "a snowman", "a sandcastle", "a picnic", "a trip",
    "a car", "a train", "a boat", "a plane", "a bus", "space and rockets", "the user's name",
    "what the user is doing", "what the user ate today", "something the user made",
]

USER_STYLES = [
    "a 6 year old kid who types in all lowercase with no punctuation",
    "a grown-up who writes neat full sentences",
    "someone who mostly sends one-word messages",
    "someone who makes small spelling mistakes",
    "a shy person who sends very short messages",
    "an excited kid who uses lots of exclamation marks",
    "a curious kid who keeps asking why",
    "someone who is a bit sad today",
    "a kind grandparent",
    "someone who loves jokes",
    "someone who asks Pixel tricky questions",
    "a kid who wants to play a game",
    "a bored teenager who types in lowercase",
    "a very polite person",
    "a tired person after a long day",
    "someone meeting Pixel for the first time",
    "someone who changes the subject often",
    "a kid who types 'u' for 'you' and 'ur' for 'your'",
    "someone who says 'lol' and 'haha' a lot",
    "a kid who just learned to count",
    "a little kid who tells Pixel about their day",
    "someone who is a little grumpy at first",
    "someone who is very happy today",
    "a person who asks lots of questions about Pixel",
    "a kid who loves animals",
    "someone who types without capital letters",
]

OPENINGS = [
    "The user starts with a greeting.",
    "The user starts with a greeting.",
    "The user starts right away with a question.",
    "The user starts by saying something about their day.",
    "The user starts with just one word.",
    "The user starts with a question about Pixel.",
    "The user starts by telling Pixel a feeling.",
]

MOODS = ["cheerful", "cheerful", "cheerful", "playful", "silly", "curious", "proud",
         "a little confused", "calm", "excited", "kind and gentle"]

EXCHANGES = [1, 2, 2, 3, 3, 3, 4, 4, 5, 6]

STOP = set("""a an the and or but if of to in on at by for with from up down out over under is are was
were be been am do does did have has had i you he she it we they me him her us them my your his its
our their this that these those there here then than so very just not no yes too also as into""".split())


def load_vocab(path):
    words = [w.strip() for w in open(path) if w.strip()]
    try:  # never suggest first names (they would appear lower-case) as random words
        sys.path.insert(0, os.path.dirname(os.path.abspath(path)))
        from build_vocab import NAMES
        names = set(NAMES)
    except Exception:
        names = set()
    content = [w for w in words if len(w) >= 3 and w not in STOP and w not in names
               and "'" not in w and not w.isdigit()]
    return words, content


def seed_card(rng, content_words):
    return {
        "topic": rng.choice(TOPICS),
        "user": rng.choice(USER_STYLES),
        "opening": rng.choice(OPENINGS),
        "mood": rng.choice(MOODS),
        "n": rng.choice(EXCHANGES),
        "words": rng.sample(content_words, 2),
    }


def card_text(i, c):
    return (f"Chat {i}: topic: {c['topic']}. The user is {c['user']}. {c['opening']} "
            f"Pixel feels {c['mood']}. Exactly {c['n']} exchange{'s' if c['n'] > 1 else ''} "
            f"(one exchange = one user message then one Pixel reply). "
            f"If they fit naturally, use the words \"{c['words'][0]}\" and \"{c['words'][1]}\".")


def build_messages(cards):
    system = ("You write short pretend chats between a user and Pixel for a picture book. "
              "The user types on Pixel's keyboard and Pixel answers on its screen.\n\n"
              + PERSONA + "\n\n" + RULES + "\n\n" + FORMAT)
    user = (f"Write {len(cards)} different chats, one for each card below.\n\n"
            + "\n".join(card_text(i + 1, c) for i, c in enumerate(cards)))
    return [{"role": "system", "content": system}, {"role": "user", "content": user}]


CHAT_SPLIT = re.compile(r"^#+\s*Chat\s*\d+\s*:?\s*$", re.M | re.I)
TURN = re.compile(r"^\s*(User|Pixel)\s*:\s*(.*)$", re.I)


def parse_chats(text):
    """Split model output into chats; each chat is a list of [role, text]. Lines that are not
    turns (blank, stray prose) end the current turn; continuation lines are rejected later."""
    out = []
    for block in CHAT_SPLIT.split(text):
        turns, bad = [], False
        for line in block.strip().splitlines():
            if not line.strip():
                continue
            m = TURN.match(line)
            if not m:
                bad = True
                break
            turns.append([m.group(1).lower(), m.group(2).strip()])
        if turns and not bad:
            out.append(turns)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--vocab", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--requests", type=int, default=10000)
    ap.add_argument("--chats-per-req", type=int, default=5)
    ap.add_argument("--batch", type=int, default=2000, help="requests per llm.chat call")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--temperature", type=float, default=0.85)
    # top_p < 1 makes vLLM 0.25 run its Triton top-k/top-p kernel, which hung (GPU spinning at 100%,
    # ~150 W) on Fir H100s on 2026-10-15; plain temperature sampling avoids it.
    ap.add_argument("--top-p", type=float, default=1.0)
    ap.add_argument("--max-tokens", type=int, default=1400)
    ap.add_argument("--max-num-seqs", type=int, default=512)
    ap.add_argument("--keep-raw", type=float, default=0.002)
    ap.add_argument("--backend", choices=["vllm", "hf"], default="vllm")
    ap.add_argument("--eager", action="store_true", help="vLLM enforce_eager (no compile / CUDA graphs)")
    ap.add_argument("--no-async", action="store_true", help="vLLM async_scheduling=False")
    ap.add_argument("--hf-batch", type=int, default=192)
    args = ap.parse_args()
    # Late overrides (applies to queued tasks without resubmitting): scripts/gen_overrides.json
    # {"args": {"eager": true, ...}, "env": {"VLLM_USE_FLASHINFER_SAMPLER": "1"}}
    ov_path = os.environ.get("GEN_OVERRIDES", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                            "gen_overrides.json"))
    if os.path.exists(ov_path):
        ov = json.load(open(ov_path))
        for k, v in ov.get("args", {}).items():
            setattr(args, k, v)
        os.environ.update({k: str(v) for k, v in ov.get("env", {}).items()})
        print("overrides:", ov, flush=True)

    rng = random.Random(args.seed * 1000003 + 17)
    _, content = load_vocab(args.vocab)
    t_init = time.time()
    if args.backend == "vllm":
        from vllm import LLM, SamplingParams
        kw = {}
        if args.eager:
            kw["enforce_eager"] = True
        if args.no_async:
            kw["async_scheduling"] = False
        llm = LLM(model=args.model, dtype="bfloat16", max_model_len=4096, gpu_memory_utilization=0.92,
                  enable_prefix_caching=True, max_num_seqs=args.max_num_seqs, seed=args.seed, **kw)
        sp = SamplingParams(temperature=args.temperature, top_p=args.top_p, max_tokens=args.max_tokens)

        def run(msgs):
            outs = llm.chat(msgs, sp, use_tqdm=False)
            return [(o.outputs[0].text, len(o.outputs[0].token_ids)) for o in outs]
    else:
        import torch
        from transformers import AutoModelForCausalLM, AutoTokenizer
        torch.manual_seed(args.seed)
        htok = AutoTokenizer.from_pretrained(args.model, padding_side="left")
        hmod = AutoModelForCausalLM.from_pretrained(args.model, dtype=torch.bfloat16,
                                                    attn_implementation="sdpa").to("cuda").eval()

        def run(msgs):
            res = []
            texts = [htok.apply_chat_template(m, tokenize=False, add_generation_prompt=True) for m in msgs]
            for i in range(0, len(texts), args.hf_batch):
                enc = htok(texts[i:i + args.hf_batch], return_tensors="pt", padding=True).to("cuda")
                with torch.no_grad():
                    out = hmod.generate(**enc, max_new_tokens=args.max_tokens, do_sample=args.temperature > 0,
                                        temperature=max(args.temperature, 1e-5), top_p=args.top_p,
                                        pad_token_id=htok.eos_token_id)
                gen_ids = out[:, enc["input_ids"].shape[1]:]
                for row in gen_ids:
                    row = row[row != htok.eos_token_id]
                    res.append((htok.decode(row, skip_special_tokens=True), int(row.numel())))
            return res
    print(f"init {time.time() - t_init:.0f}s backend={args.backend} eager={args.eager} "
          f"no_async={args.no_async} temp={args.temperature} top_p={args.top_p}", flush=True)

    # resume: count requests already written (each line carries its request index)
    done = set()
    if os.path.exists(args.out):
        for line in open(args.out):
            try:
                done.add(json.loads(line)["req"])
            except Exception:
                pass
    all_cards = [[seed_card(rng, content) for _ in range(args.chats_per_req)]
                 for _ in range(args.requests)]
    todo = [i for i in range(args.requests) if i not in done]
    print(f"requests: {args.requests}, already done: {len(done)}, todo: {len(todo)}", flush=True)

    t0, gen_tok, n_chat, n_req = time.time(), 0, 0, 0
    with open(args.out, "a") as f:
        for b in range(0, len(todo), args.batch):
            idx = todo[b:b + args.batch]
            msgs = [build_messages(all_cards[i]) for i in idx]
            outs = run(msgs)
            for i, (text, ntok) in zip(idx, outs):
                gen_tok += ntok
                chats = parse_chats(text)
                cards = all_cards[i]
                for k, turns in enumerate(chats):
                    rec = {"id": f"s{args.seed}r{i}c{k}", "req": i,
                           "seed": cards[k] if k < len(cards) else None, "turns": turns}
                    f.write(json.dumps(rec) + "\n")
                    n_chat += 1
                if not chats:  # keep a marker so resume skips it
                    f.write(json.dumps({"id": f"s{args.seed}r{i}x", "req": i, "seed": None,
                                        "turns": [], "raw": text[:2000]}) + "\n")
                elif rng.random() < args.keep_raw:
                    f.write(json.dumps({"id": f"s{args.seed}r{i}raw", "req": i, "seed": None,
                                        "turns": [], "raw": text}) + "\n")
                n_req += 1
            f.flush()
            dt = time.time() - t0
            print(f"[{time.strftime('%H:%M:%S')}] req {n_req}/{len(todo)} chats {n_chat} "
                  f"gen_tok {gen_tok} ({gen_tok / dt:.0f} tok/s)", flush=True)
    print(f"DONE requests={n_req} chats={n_chat} gen_tokens={gen_tok} secs={time.time() - t0:.0f}")


if __name__ == "__main__":
    sys.exit(main())

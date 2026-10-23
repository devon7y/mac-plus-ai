#!/usr/bin/env python3
"""Clean, filter and deduplicate raw Pixel chats (output of gen.py).

Checks (a chat is dropped on the first failure; reasons are counted):
  shape      roles alternate user/pixel, start with user, end with pixel, 1-8 exchanges
  ascii      after normalising quotes/dashes/ellipses, only printable ASCII; no markup (* # < > [ ] _ ~ | \\ { })
  len        Pixel turn <= 22 words and <= 3 sentences; user turn <= 20 words; no empty turn
  vocab      every Pixel word is in vocab.txt, or (at most 2 per turn) echoes a word the user typed
             earlier in the chat; user turns have at most 2 out-of-vocabulary words
  persona    Pixel never says AI/assistant/program/model/robot-assistant phrases, never names a year
             other than 1986, never claims modern tech; no 'User:'/'Pixel:' leakage
  dup        exact duplicate chats (case/punctuation-insensitive) and over-used first exchanges
             (> --max-first copies of the same normalised opening user+Pixel pair)

Usage: python3 filter.py --vocab vocab.txt --out clean.jsonl --stats stats.json raw_*.jsonl
"""
import argparse
import collections
import glob
import hashlib
import json
import re
import sys

WORD = re.compile(r"[A-Za-z]+(?:'[A-Za-z]+)*")
NORM_MAP = {
    "‘": "'", "’": "'", "‚": "'", "‛": "'", "′": "'",
    "“": '"', "”": '"', "„": '"', "″": '"',
    "–": "-", "—": " - ", "―": " - ", "−": "-",
    "…": "...", " ": " ", " ": " ", "​": "",
}
BAD_CHARS = set("*#<>[]_~|\\{}`^@$%&=+")
PERSONA_BAD = re.compile(
    r"\b(ai|a\.i\.|artificial|intelligence|language model|assistant|chatbot|chat bot|qwen|alibaba|"
    r"openai|chatgpt|gpt|program|programmed|software|algorithm|digital|virtual|online|internet|"
    r"wifi|wi-fi|website|smartphone|iphone|android|tablet|app|apps|email|google|youtube|tiktok|"
    r"instagram|facebook|netflix|siri|alexa|download|upload|update|cloud storage|data|"
    r"don't have feelings|do not have feelings|can't feel|cannot feel|as a computer, i)\b", re.I)
YEAR = re.compile(r"\b(1[0-9]{3}|20[0-9]{2})\b")
SENT = re.compile(r"[.!?]+")


def normalise(s):
    for a, b in NORM_MAP.items():
        s = s.replace(a, b)
    s = re.sub(r"\s+", " ", s).strip()
    # strip wrapping quotes the model sometimes adds around a whole turn
    if len(s) >= 2 and s[0] == '"' and s[-1] == '"' and s.count('"') == 2:
        s = s[1:-1].strip()
    return s


def words(s):
    return [w.lower() for w in WORD.findall(s)]


def in_vocab(w, vocab):
    if w in vocab:
        return True
    # possessive / simple plural of an allowed word
    if w.endswith("'s") and w[:-2] in vocab:
        return True
    if w.endswith("s") and w[:-1] in vocab and len(w) > 3:
        return True
    return False


def check(turns, vocab, oov_counter):
    if not turns or len(turns) % 2 or len(turns) > 16:
        return "shape", None
    for i, (r, _) in enumerate(turns):
        if r != ("user" if i % 2 == 0 else "pixel"):
            return "shape", None
    clean, user_words = [], set()
    for r, t in turns:
        t = normalise(t)
        if not t:
            return "len", None
        if any(ord(c) < 32 or ord(c) > 126 for c in t) or any(c in BAD_CHARS for c in t):
            return "ascii", None
        if re.search(r"\b(user|pixel)\s*:", t, re.I):
            return "persona", None
        ws = words(t)
        if r == "user":
            if len(t.split()) > 20:
                return "len", None
            if sum(1 for w in ws if not in_vocab(w, vocab)) > 2:
                return "vocab_user", None
            user_words.update(ws)
        else:
            if len(t.split()) > 22 or len([s for s in SENT.split(t) if s.strip()]) > 3:
                return "len", None
            if PERSONA_BAD.search(t):
                return "persona", None
            if any(y != "1986" for y in YEAR.findall(t)):
                return "persona", None
            oov = [w for w in ws if not in_vocab(w, vocab)]
            echo = [w for w in oov if w in user_words]
            if len(oov) > len(echo) or len(echo) > 2:
                for w in oov:
                    if w not in user_words:
                        oov_counter[w] += 1
                return "vocab_pixel", None
        clean.append([r, t])
    return None, clean


def key(turns, n=None):
    s = " | ".join(t for _, t in (turns if n is None else turns[:n]))
    return hashlib.md5(re.sub(r"[^a-z0-9|]+", "", s.lower()).encode()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vocab", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--stats", required=True)
    ap.add_argument("--max-first", type=int, default=20)
    ap.add_argument("inputs", nargs="+")
    args = ap.parse_args()
    vocab = {w.strip() for w in open(args.vocab) if w.strip()}
    paths = sorted(p for pat in args.inputs for p in glob.glob(pat))
    reasons, oov = collections.Counter(), collections.Counter()
    seen, first = set(), collections.Counter()
    n_in = n_out = 0
    pixel_words = user_words = 0
    with open(args.out, "w") as fo:
        for p in paths:
            for line in open(p):
                try:
                    rec = json.loads(line)
                except Exception:
                    reasons["json"] += 1
                    continue
                if not rec.get("turns"):
                    continue
                n_in += 1
                why, clean = check(rec["turns"], vocab, oov)
                if why:
                    reasons[why] += 1
                    continue
                k = key(clean)
                if k in seen:
                    reasons["dup"] += 1
                    continue
                k1 = key(clean, 2)
                if first[k1] >= args.max_first:
                    reasons["dup_first"] += 1
                    continue
                seen.add(k)
                first[k1] += 1
                for r, t in clean:
                    if r == "pixel":
                        pixel_words += len(t.split())
                    else:
                        user_words += len(t.split())
                fo.write(json.dumps({"id": rec["id"], "seed": rec.get("seed"), "turns": clean}) + "\n")
                n_out += 1
    stats = {"inputs": paths, "chats_in": n_in, "chats_out": n_out,
             "keep_rate": round(n_out / max(n_in, 1), 4), "rejects": dict(reasons.most_common()),
             "pixel_words": pixel_words, "user_words": user_words,
             "top_pixel_oov": oov.most_common(300)}
    json.dump(stats, open(args.stats, "w"), indent=1)
    print(json.dumps({k: v for k, v in stats.items() if k != "top_pixel_oov"}, indent=1))
    print("top Pixel OOV:", ", ".join(f"{w}:{c}" for w, c in oov.most_common(60)))


if __name__ == "__main__":
    sys.exit(main())

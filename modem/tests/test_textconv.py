#!/usr/bin/env python3
"""Differential test: the firmware's textconv.cpp against the relay's askai/relay/textconv.py.

    python3 modem/tests/test_textconv.py [--seed N] [--cases N]

Builds modem/tests/tc_cli.cpp with clang++ into modem/build/tc/, then feeds the same input to both
and compares bytes:

* stream: Markdown cases (askai's test_relay.py, the mock provider's replies) fed whole, code point
  by code point, byte by byte and in random chunks; a seeded fuzzer of Markdown-ish text (also with
  invalid UTF-8); and a sweep of "a X b X c" lines for every code point up to U+2FFFF (plus a sample
  above), which checks _drop_symbols' table. Output is the relay's wire form after pack().
* tomac: to_mac on every code point U+0000..U+10FFFF (no surrogates) and on whole fuzz strings.
* frommac: from_mac on every byte and on random byte strings with CR/LF.
* decode: UTF-8 decoding with U+FFFD for invalid input, against bytes.decode("utf-8", "replace").

Prints a summary; exits 1 on any mismatch (showing the first few). Python stdlib only.
"""
import argparse
import codecs
import os
import random
import struct
import subprocess
import sys
import time

sys.dont_write_bytecode = True                       # askai/ is read-only: no __pycache__ there
HERE = os.path.dirname(os.path.abspath(__file__))
MODEM = os.path.dirname(HERE)
ROOT = os.path.dirname(MODEM)
sys.path.insert(0, os.path.join(ROOT, "askai", "relay"))
sys.path.insert(0, os.path.join(ROOT, "askai", "relay", "tests"))
import providers  # noqa: E402
import test_relay  # noqa: E402
import textconv  # noqa: E402

FW = os.path.join(MODEM, "firmware", "plusmodem")
BUILD = os.path.join(MODEM, "build", "tc")
CLI = os.path.join(BUILD, "tc_cli")
END = struct.pack("<I", 0xFFFFFFFF)


def build():
    os.makedirs(BUILD, exist_ok=True)
    src = os.path.join(FW, "textconv.cpp")
    for cmd in (["clang++", "-std=c++11", "-Wall", "-Wextra", "-O1", "-o", CLI,
                 os.path.join(HERE, "tc_cli.cpp"), src],
                ["clang++", "-std=gnu++11", "-fno-rtti", "-fno-exceptions", "-Wall", "-Wextra", "-O1", "-c",
                 "-o", os.path.join(BUILD, "textconv_noexc.o"), src]):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode or r.stderr.strip():
            print(" ".join(cmd) + "\n" + r.stderr)
            if r.returncode:
                sys.exit(1)


def rec(b):
    return struct.pack("<I", len(b)) + b


def run_cli(mode, payload):
    r = subprocess.run([CLI, mode], input=payload, capture_output=True, check=True)
    out, i, d = [], 0, r.stdout
    while i < len(d):
        n = struct.unpack_from("<I", d, i)[0]
        out.append(d[i + 4:i + 4 + n])
        i += 4 + n
    return out


# ------------------------------------------------------------------------------------ reference

def wire(evs):
    return b"".join(b"N\r" if e[0] == "N" else b"T " + textconv.to_mac(e[1]) + b"\r" for e in evs)


def py_stream(chunks):
    """What the relay does with these UTF-8 chunks (an incremental decoder stands in for the
    firmware holding back a cut-off multi-byte sequence)."""
    dec = codecs.getincrementaldecoder("utf-8")("replace")
    ps = textconv.PlainStream()
    evs = []
    for c in chunks:
        evs += ps.feed(dec.decode(c))
    tail = dec.decode(b"", True)
    if tail:
        evs += ps.feed(tail)
    evs += ps.finish()
    return wire(textconv.pack(evs))


def split(data, how, rng):
    if how == "whole":
        return [data]
    if how == "bytes":
        return [data[i:i + 1] for i in range(len(data))]
    if how == "chars":                               # at code point boundaries
        cuts = [i for i in range(1, len(data)) if not 0x80 <= data[i] <= 0xBF]
    else:
        top = 16 if how == "small" else 80
        cuts, i = [], 0
        while True:
            i += rng.randint(1, top)
            if i >= len(data):
                break
            cuts.append(i)
    edges = [0] + cuts + [len(data)]
    return [data[a:b] for a, b in zip(edges, edges[1:])]


# ---------------------------------------------------------------------------------------- cases

def fixed_cases():
    t = test_relay.TestText
    md = [("test_relay.MD", t.MD),
          ("word*200", "word " * 200 + "\n"),
          ("mac_roman", "“Hi” — café → 5×3 € •"),
          ("cjk", "日"),
          ("emoji", "Nice \U0001F600 work \U0001F44D\U0001F3FD!")]
    for i, r in enumerate(providers.MOCK_REPLIES):
        md.append(("MOCK_REPLIES[%d]" % i, r))
        md.append(("mock reply %d" % i, 'You said: "hello there"\n\n' + r + "\n\n(Mock model: mock-1, turn %d.)" % i))
    md.append(("MOCK_LONG", providers.MOCK_LONG))
    # boundaries: pack's "split after a blank past half", _open_link's 300 and 400 limits
    md.append(("pack edge", "\n".join("a" * k + " " + "b" * (250 - k) for k in range(85, 105))))
    md.append(("open [ edge", "\n".join("x [" + "w " * n + "](u) y" for n in range(145, 155))))
    md.append(("open ( edge", "\n".join('x [a](u "' + "t " * n + '") y' for n in range(190, 202))))
    given = [("streams_word_by_word", [c.encode() for c in ("Hello wor", "ld and", " a [link", "](u) x")])]
    return md, given


WORDS = ("the a Mac Plus modem serial line reply text code snake_case x_y foo_bar 2 3 6 = + - / and or 1986 "
         "hello world it's don't e.g. A.B. 3.14 50% #tag @me $5 a/b").split()
INLINE = ["**", "*", "_", "__", "~~", "~", "`", "``", "***", "**bold**", "*it*", "_em_", "~~gone~~", "`code`",
          "`a b`", "(**x**)", '"*q*"', "**a**,", "*b*.", "x**y**z", "‘*s*’", "“**d**”", "**x**)!", "*__y__*",
          "~~~z~~~", "~a", "a~", "_*x*_", "****", "*a", "b*", "(*", "[**", "**)", "*)", "'_x_'", "**”", "‘**"]
LINKS = ["[text](http://a.b/c)", '[x](u "t")', '[x](u  "t" )', "![img](p.png)", "[a]", "[](u)", "[u](u)",
         "[m](mailto:m)", "[ sp ](u)", "[a](b c)", "<https://example.com/x>", "<mailto:a@b.c>", "<ftp://x>",
         "<http:>", "<http://a b>", "<https://x.y/z?q=1&r=2>", "[", "]", "(", ")", "](", "[a](", "[a]x", "![",
         "[x](<y>)", "[*b*](u)", "[a\\]](u)", "[ a ](u)", '[t](u\t"z")', "[a](u)[b](v)", "!", "<", ">"]
ESC = ["\\*", "\\_", "\\\\", "\\`", "\\[", "\\]", "\\#", "\\>", "\\a", "\\", "\\|", "\\~", "\\-", "\\.", "\\!"]
UNI = ["😀", "👍🏽", "❤️", "⭐", "👨‍👩‍👧", "✓", "✔", "✗", "✘", "→", "←", "⇒", "↔", "⇔", "↑", "↓", "×", "€", "¤", "“",
       "”", "‘", "’", "„", "‚", "–", "—", "―", "‐", "‑", "−", "…", " ", " ", " ", "　", "café",
       "naïve", "Ünïcödé", "Łódź", "đ", "ħ", "ǅ", "ﬁ", "Ⅻ", "①", "²", "³", "½", "™", "℠", "№", "日本語", "中文",
       "Привет", "Ελληνικά", "∑", "∫", "≤", "≥", "≠", "≈", "∞", "√", "π", "Ω", "é", "ä", "‍",
       "​", "﻿", "­", "️", "︎", "‌", "⁠", "\x7f", "\x00", "\x01", "\x0b", "\x0c",
       "\x1c", "\x1f", "\u0085", " ", " ", "１", "٣", "𝐀𝐁𝐂", "ｆｕｌｌ", "\U000E0041", "",
       "", "한국어", "مرحبا", "ไทย", "ı", "ŀ", "̀", "⃝", "¼", "‰", "‹", "›", "•", "●", "▪", "◦",
       "‣", "⁃", "─", "│", "┌", "┼", "\t", "�", "\U0001F1FA\U0001F1F8", "©", "®", "°", "§", "¶", "†", "ß",
       "æ", "ø", "Ÿ", "ƒ", "Δ", "◊", "ﬂ", "ˆ", "˜", "¯", "˘", "˙", "˚", "¸", "˝", "˛", "ˇ", "′", "″", "℃",
       " ", " ", "😀 ", " 😀", "\U0010FFFF", "\U0001D7CE", "ǈ", "ﾊ", "㍿", "⑴", "⒜", "ℌ", "́x"]
PREFIX = ["- ", "* ", "+ ", "1. ", "12) ", "1234567890. ", "123456789. ", "  - ", "    * ", "-  ", "-", "*",
          "# ", "## ", "###### ", "####### ", "#", "#x ", "> ", ">", "   # ", "    # ", "１. ", "٣) ", "1.",
          "---", "***", "___", "- - -", "* * *", "_ _ _ _", "--", "  ---  ", "-*-", "===", "  ==", "=", "= =",
          "    ---", "-- ", "**", "*bold* ", "~", "`", "~~", " - ", " "]
FENCES = ["```", "```py", "~~~", "````", "  ```", "    ```", "``", "~~~~ x", "```` ```", "``` `"]
TABLES = ["| a | b |", "|---|---|", "|:--|--:|", "--- | ---", "a | b", "|a|", "| - |", "||", "|---||", ":-: | :-:",
          " | --- | ", "|-- -|", "| `a | b` |", "|**x**|*y*|", "| [l](u) | 😀 |", "|", "a |", "| a", "a|b|",
          "|:-:|", "| -- | :: |", "|-|-|-|", "- | -", "|--- |---", "|---|---| x"]
SEP = [" "] * 12 + ["", "", "", "  ", "\t", "    "]
EOL = ["\n"] * 10 + ["\n\n", "\r\n", "\r", "\n\n\n", "\n   \n", " \n"]
BAD = [b"\xff", b"\xc3", b"\xe2\x82", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xc0\xaf", b"\x80", b"\xf0\x9f\x98",
       b"\xe0\x80", b"\xf5"]


def fuzz_doc(rng):
    lines = []
    for _ in range(rng.randint(1, 10)):
        r = rng.random()
        if r < 0.08:                                 # a code block
            f = rng.choice(FENCES)
            lines.append(f)
            for _ in range(rng.randint(0, 3)):
                lines.append(rng.choice(["  ", "", "    "]) + " ".join(rng.choice(WORDS + INLINE + UNI)
                                                                 for _ in range(rng.randint(0, 5))))
            if rng.random() < 0.8:
                lines.append(rng.choice([f, f.strip() + "`", "~~~", "```", "  " + f]))
            continue
        if r < 0.15:                                 # a table
            lines += [rng.choice(TABLES) for _ in range(rng.randint(1, 4))]
            continue
        if r < 0.18:                                 # a long line (pack splits it)
            lines.append(" ".join(rng.choice(WORDS + UNI[:30]) for _ in range(rng.randint(40, 120))))
            continue
        if r < 0.20:                                 # a long unfinished link or word
            lines.append(rng.choice(["[", "a [b](", "x"]) + rng.choice(["w ", "w", "é "]) * rng.randint(100, 450))
            continue
        line = rng.choice(PREFIX) if rng.random() < 0.4 else ""
        for _ in range(rng.randint(0, 14)):
            k = rng.random()
            pool = WORDS if k < 0.45 else INLINE if k < 0.6 else LINKS if k < 0.7 else ESC if k < 0.75 else UNI
            line += rng.choice(pool) + rng.choice(SEP)
        lines.append(line)
    text = "".join(ln + rng.choice(EOL) for ln in lines)
    if rng.random() < 0.3:
        text = text.rstrip("\n")
    data = text.encode("utf-8", "surrogatepass")
    if rng.random() < 0.05:                          # invalid UTF-8 somewhere (or cut off at the end)
        for _ in range(rng.randint(1, 3)):
            i = rng.randint(0, len(data))
            data = data[:i] + rng.choice(BAD) + data[i:]
    return text, data


def sweep_lines():
    cps = [cp for cp in range(0x30000) if not 0xD800 <= cp < 0xE000]
    cps += list(range(0xE0000, 0xE0200)) + list(range(0x30000, 0x110000, 97)) + [0xFFFFD, 0x10FFFD, 0x10FFFF]
    lines = ["a %s b %s c\n" % (chr(cp), chr(cp)) for cp in cps]
    return [("sweep %d" % i, "".join(lines[i:i + 4096]).encode()) for i in range(0, len(lines), 4096)]


# ------------------------------------------------------------------------------------------ main

def show(b, at):
    return repr(b[max(0, at - 60):at + 60])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cases", type=int, default=3000, help="fuzz cases (default 3000)")
    a = ap.parse_args()
    t0 = time.time()
    build()
    rng = random.Random(a.seed)
    fails = []

    # ---- stream
    cases = []                                       # (name, chunks)
    md, given = fixed_cases()
    for name, s in md:
        data = s.encode()
        for how in ("whole", "chars", "bytes", "small", "small", "big"):
            cases.append(("%s/%s" % (name, how), split(data, how, rng)))
    cases += given
    for i in range(a.cases):
        _, data = fuzz_doc(rng)
        how = rng.choice(["whole", "whole", "chars", "bytes", "small", "small", "small", "big", "big"])
        cases.append(("fuzz %d/%s" % (i, how), split(data, how, rng)))
    nfuzz_end = len(cases)
    cases += [(n, [d]) for n, d in sweep_lines()]
    payload = b"".join(b"".join(rec(c) for c in chunks) + END for _, chunks in cases)
    got = run_cli("stream", payload)
    assert len(got) == len(cases), (len(got), len(cases))
    for (name, chunks), g in zip(cases, got):
        want = py_stream(chunks)
        if g != want:
            at = next((i for i in range(min(len(g), len(want))) if g[i] != want[i]), min(len(g), len(want)))
            fails.append("stream %s\n  input: %r\n  python: %s\n  c++:    %s"
                         % (name, b"".join(chunks)[:400], show(want, at), show(g, at)))
    n_stream = len(cases)

    # ---- tomac: every code point, then whole strings
    cps = [cp for cp in range(0x110000) if not 0xD800 <= cp < 0xE000]
    strs = [chr(cp) for cp in cps]
    for _ in range(3000):
        text, _ = fuzz_doc(rng)
        strs.append(text.replace("\n", " ").replace("\r", " ")[:300])
    strs += ["a\x7fb", "a\x7f€", "\x7f\x00", "\x7f ", "¤", "a¤b", "•​", "Ä\x7f", "\x7f\U0001F600"]
    got = run_cli("tomac", b"".join(rec(s.encode()) for s in strs))
    bad = 0
    for s, g in zip(strs, got):
        want = textconv.to_mac(s)
        if g != want:
            bad += 1
            if bad <= 5:
                fails.append("to_mac %r (%s): python %r, c++ %r" % (s[:80], " ".join("U+%04X" % ord(c) for c in s[:4]),
                                                                 want, g))
    if bad > 5:
        fails.append("to_mac: %d mismatches in all" % bad)
    n_tomac = len(strs)

    # ---- frommac
    blobs = [bytes([b]) for b in range(256)] + [b"caf\x8e\rnext", b"\r\n\r\r\n\n\r", b"a\r", b"\ra"]
    for _ in range(2000):
        blobs.append(bytes(rng.choice([13, 10, 32, 65, 0x8E, 0xDB, 0xF0, rng.randrange(256)])
                           for _ in range(rng.randint(0, 40))))
    got = run_cli("frommac", b"".join(rec(b) for b in blobs))
    for b, g in zip(blobs, got):
        want = textconv.from_mac(b).encode()
        if g != want:
            fails.append("from_mac %r: python %r, c++ %r" % (b, want, g))

    # ---- decode
    blobs = [bytes([b]) for b in range(256)]
    pieces = [b"a", b"\x80", b"\xbf", b"\xc2", b"\xdf", b"\xe0", b"\xed", b"\xef", b"\xf0", b"\xf4", b"\xf5", b"\xff",
              b"\xa0", b"\x9f", b"\x90", b"\x8f", "é".encode(), "€".encode(), "😀".encode()] + BAD
    for _ in range(5000):
        blobs.append(b"".join(rng.choice(pieces) for _ in range(rng.randint(1, 12))))
    got = run_cli("decode", b"".join(rec(b) for b in blobs))
    for b, g in zip(blobs, got):
        want = b.decode("utf-8", "replace").encode()
        if g != want:
            fails.append("utf8 decode %r: python %r, c++ %r" % (b, want, g))

    print("textconv.cpp vs textconv.py (Python %s, Unicode %s, seed %d): %d stream cases (%d fuzz, %d sweep), "
          "%d to_mac, %d from_mac, %d decode checks in %.1f s"
          % (sys.version.split()[0], __import__("unicodedata").unidata_version, a.seed, n_stream,
             a.cases, n_stream - nfuzz_end, n_tomac, 256 + 2004, 256 + 5000, time.time() - t0))
    if fails:
        print("%d MISMATCHES; first ones:" % len(fails))
        for f in fails[:8]:
            print(f)
        sys.exit(1)
    print("all match")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Compare the Mac's Finish it runs (dumped by test_finish.lua) with paint/bench/ref.py:
the prompt rows built from the recorded points, and every generated row.

Usage: check_m5.py <model.dmdl> <out dir> [expected UI category name]
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "bench"))
import model_file  # noqa: E402
import ref  # noqa: E402

m = model_file.read(sys.argv[1])
out = sys.argv[2]
ok = True
for tag in ("mb", "ui"):
    info = open(os.path.join(out, f"{tag}_info.txt")).read().split()
    kv = {info[i]: int(info[i + 1]) for i in range(0, len(info), 2)}
    w = lambda name: struct.unpack(f">{len(open(os.path.join(out, name), 'rb').read()) // 2}h",
                                   open(os.path.join(out, name), "rb").read())
    pts = w(f"{tag}_points.bin")
    pts = [tuple(pts[i:i + 3]) for i in range(0, len(pts), 3)]
    mac_prompt = w(f"{tag}_prompt.bin")
    mac_prompt = [tuple(mac_prompt[i:i + 3]) for i in range(0, len(mac_prompt), 3)]
    mac_gen = w(f"{tag}_gen.bin")
    mac_gen = [tuple(mac_gen[i:i + 3]) for i in range(0, len(mac_gen), 3)]
    # the UI run's recorded points include the first completion's? No: it was undone.
    start, rows = ref.prompt_rows(pts)
    good_p = rows == mac_prompt
    gen = ref.finish_it(m, rows, kv["cat"], kv["seed"], 1)
    good_g = gen == mac_gen
    ok &= good_p and good_g
    ticks_prime, ticks_draw = kv["t1"] - kv["t0"], kv["t2"] - kv["t1"]
    print(f"[{tag}] category {kv['cat']} ({m['names'][kv['cat']]}), seed {kv['seed']}: prompt rows "
          f"{'MATCH' if good_p else 'DIFFER'} ({len(rows)} rows from {len(pts)} points); generated rows "
          f"{'MATCH' if good_g else 'DIFFER'} ({len(mac_gen)} Mac, {len(gen)} reference)")
    print(f"      priming {kv['primed']} inputs in {ticks_prime / 60.15:.2f} s "
          f"({ticks_prime / 60.15 / max(1, kv['primed']):.3f} s each); drawing {len(mac_gen)} moves in "
          f"{ticks_draw / 60.15:.2f} s ({ticks_draw / 60.15 / max(1, len(mac_gen)):.3f} s per move); "
          f"FreeMem after loading {kv['freemem']:,}; model RAM {kv.get('modelbytes', 0):,}, "
          f"loaded in {kv.get('load', 0) / 60.15:.1f} s from "
          f"{ {1: 'the app folder', 2: 'the default folder', 3: 'the volume root'}.get(kv.get('where'), '?') }")
    if not good_g:
        for i, (a, b) in enumerate(zip(gen, mac_gen)):
            if a != b:
                print(f"      first difference at move {i}: reference {a}, Mac {b}")
                break
    if tag == "ui" and len(sys.argv) > 3:
        good_c = m["names"][kv["cat"]] == sys.argv[3]
        ok &= good_c
        print(f"      picked category '{m['names'][kv['cat']]}' {'==' if good_c else '!='} '{sys.argv[3]}'")
print("M5 BIT-EXACT PASS" if ok else "M5 FAIL")
sys.exit(0 if ok else 1)

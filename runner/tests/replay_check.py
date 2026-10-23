#!/usr/bin/env python3
"""Compare the 68000's replay checkpoints (MAME output on stdin) with game.py's prediction.
    emu/run_test.sh ... replay.lua | python3 runner/tests/replay_check.py runner/build/<tag>/gen/replay.json"""
import json, sys
exp = json.load(open(sys.argv[1]))["checkpoints"]
got = {}
for line in sys.stdin:
    print(line.rstrip())
    p = line.split()
    if p and p[0] == "CP":
        got[p[1]] = [int(v) for v in p[2:]]
fails = 0
for k, e in sorted(exp.items(), key=lambda kv: int(kv[0])):
    if k not in got:
        print(f"tick {k}: missing"); fails += 1; continue
    s, c, z, st = got[k]
    want = [e["score"], e["coins"], e["z"], (e["state"] << 16) | (e["lane"] << 8) | (e["y"] >> 4)]
    ok = [s, c, z, st] == want
    print(f"tick {k:>5}: {'ok  ' if ok else 'DIFF'} mac={[s, c, z, hex(st)]} python={[want[0], want[1], want[2], hex(want[3])]}")
    fails += not ok
print("RESULT", "PASS" if fails == 0 else f"FAIL ({fails})")
sys.exit(1 if fails else 0)

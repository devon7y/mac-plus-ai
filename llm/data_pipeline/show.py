#!/usr/bin/env python3
"""Print random chats from a clean/raw JSONL file for reading. Usage: show.py file.jsonl [n] [seed]"""
import json
import random
import sys

recs = [json.loads(l) for l in open(sys.argv[1])]
recs = [r for r in recs if r.get("turns")]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 20
random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 0).shuffle(recs)
for r in recs[:n]:
    s = r.get("seed") or {}
    print(f"--- {r['id']}  [{s.get('topic')}] [{s.get('user')}] [{s.get('mood')}]")
    for role, t in r["turns"]:
        print(f"  {'U' if role == 'user' else 'P'}: {t}")

#!/usr/bin/env python3
"""Summarise eval/<run>/metrics.json files into a Markdown table (stdout) and results.json.

  python collect.py <eval_dir> [<out_json>]
"""
import json
import os
import sys

ev = sys.argv[1]
rows = []
for name in sorted(os.listdir(ev)):
    p = os.path.join(ev, name, "metrics.json")
    if not os.path.exists(p) or name.startswith("smoke"):
        continue
    m = json.load(open(p))
    ms = {(d["A"], d["wrap"]): d for d in m.get("mac_sim", [])}
    rows.append({
        "run": name, "H": m["H"], "quant": m["quant"], "step": m["step"],
        "params": m["nparam"],
        "val": m["val_full"]["loss"], "val_off": m["val_full"]["off"],
        "val_pen": m["val_full"]["pen"], "test": m["test_full"]["loss"],
        "sub_float": m["sim_cudnn"]["loss"],
        "A_auto": m.get("A_auto"), "mac_auto": m.get("mac_auto", {}).get("loss"),
        "mac127": ms.get((127, True), {}).get("loss"),
        "mac63": ms.get((63, True), {}).get("loss"),
        "mac31": ms.get((31, True), {}).get("loss"),
        "ovf_rec127": ms.get((127, True), {}).get("rec_overflow_rate"),
        "ovf_out127": ms.get((127, True), {}).get("out_overflow_rate"),
        "ovf_rec63": ms.get((63, True), {}).get("rec_overflow_rate"),
        "grid_moves": m["grid_mean_moves"], "grid_hit_max": m["grid_hit_max"],
        "leave_frame": m["completions_leaving_frame"],
    })
rows.sort(key=lambda r: (r["H"], {"fp": 0, "tern": 1, "tern_g128": 2, "tern_g64": 3, "tern_r2o": 4, "tern_o4": 5, "int4": 6}[r["quant"]], r["run"]))
f = lambda v, d=4: "" if v is None else (f"{v:.{d}f}" if isinstance(v, float) else str(v))
print("| Run | H | Weights | Steps | Val loss | (offsets + pen) | Test loss | Subset: float | "
      "Mac sim, A_auto | A_auto | Mac sim A=127 wrapped | Overflow rec/out at A=127 | Moves/sample |")
print("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
for r in rows:
    print(f"| {r['run']} | {r['H']} | {r['quant']} | {r['step']} | {f(r['val'])} | "
          f"{f(r['val_off'], 3)} + {f(r['val_pen'], 3)} | {f(r['test'])} | {f(r['sub_float'])} | "
          f"{f(r['mac_auto'])} | {f(r['A_auto'])} | {f(r['mac127'])} | "
          f"{f(r['ovf_rec127'], 5)} / {f(r['ovf_out127'], 5)} | {f(r['grid_moves'], 1)} |")
if len(sys.argv) > 2:
    json.dump(rows, open(sys.argv[2], "w"), indent=1)

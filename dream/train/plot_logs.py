#!/usr/bin/env python3
"""Loss curves for a Dream run from its log files (teacher log.jsonl, student log_float/log_qat.jsonl).

  python3 plot_logs.py --teacher runs/teacher/log.jsonl --student runs/student --out curves.png
"""
import argparse
import json
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

BLUE = ["#b7d3f6", "#86b6ef", "#5598e7", "#2a78d6", "#1c5cab", "#104281", "#0d366b"]  # light -> dark
CAT = ["#2a78d6", "#eb6834"]
INK, MUTED, GRID = "#0b0b0b", "#52514e", "#e6e5e1"


def read(p):
    if not p or not os.path.exists(p):
        return []
    return [json.loads(l) for l in open(p) if l.strip()]


def style(ax, title, ylabel):
    ax.set_title(title, fontsize=10, color=INK, loc="left")
    ax.set_ylabel(ylabel, fontsize=8, color=MUTED)
    ax.set_xlabel("iteration", fontsize=8, color=MUTED)
    ax.tick_params(labelsize=7, colors=MUTED)
    ax.grid(True, color=GRID, linewidth=0.6)
    for s in ax.spines.values():
        s.set_color(GRID)


def ramp(n):
    idx = [round(i * (len(BLUE) - 1) / max(1, n - 1)) for i in range(n)]
    return [BLUE[i] for i in idx]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--teacher")
    ap.add_argument("--student")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    T = read(a.teacher)
    F = read(os.path.join(a.student, "log_float.jsonl")) if a.student else []
    Q = read(os.path.join(a.student, "log_qat.jsonl")) if a.student else []
    panels = [p for p in ("teacher", "teacher_t", "float", "qat", "eval") if
              (p.startswith("teacher") and T) or (p == "float" and F) or (p == "qat" and Q) or (p == "eval" and (F or Q))]
    fig, axs = plt.subplots(1, len(panels), figsize=(4.2 * len(panels), 3.2), squeeze=False)
    for ax, p in zip(axs[0], panels):
        if p == "teacher":
            ax.plot([r["step"] for r in T], [r["loss"] for r in T], color=CAT[0], lw=2)
            ax.set_yscale("log")
            style(ax, "Teacher v-loss", "MSE (log)")
        elif p == "teacher_t":
            nb = len(T[0]["loss_t"])
            for b, c in zip(range(nb), ramp(nb)):
                ax.plot([r["step"] for r in T], [r["loss_t"][b] for r in T], color=c, lw=2,
                        label=f"t {b / nb:.1f}-{(b + 1) / nb:.1f}")
            ax.set_yscale("log")
            ax.legend(fontsize=7, frameon=False)
            style(ax, "Teacher v-loss by noise level", "MSE (log)")
        elif p in ("float", "qat"):
            L = F if p == "float" else Q
            n = len(L[0]["mse_step"])
            for i, c in zip(range(n), ramp(n)):
                ax.plot([r["step"] for r in L], [r["mse_step"][i] for r in L], color=c, lw=2, label=f"step {i}")
            ax.set_yscale("log")
            ax.legend(fontsize=7, frameon=False, ncol=2)
            style(ax, f"Student ({p}) x0 MSE per sampler step", "MSE vs target (log)")
        else:
            off = F[-1]["step"] if F else 0          # QAT continues after the float stage
            for L, lab, c, o in ((F, "float", CAT[0], 0), (Q, "qat", CAT[1], off)):
                E = [r for r in L if "eval_mse_vs_teacher" in r]
                if E:
                    ax.plot([o + r["step"] for r in E], [r["eval_mse_vs_teacher"] for r in E], color=c, lw=2,
                            marker="o", ms=4, label=lab)
            ax.legend(fontsize=7, frameon=False)
            style(ax, "Final picture vs teacher (same noise)", "MSE")
            ax.set_xlabel("iteration (QAT after float)", fontsize=8, color=MUTED)
    fig.tight_layout()
    fig.savefig(a.out, dpi=110, facecolor="white")
    print(a.out)


if __name__ == "__main__":
    main()

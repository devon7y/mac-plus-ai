# Chatbot speed benchmark: results (2026-10-15)

Real 68000 kernels for the chosen model (option A: 6.7M-parameter ternary mixture of experts;
6 layers, width 160, 4 query / 2 key-value heads, 4 experts with one active, 2,048-token 4-bit
vocabulary table, 4-bit keys/values) timed on MAME's Mac Plus with 4 MB, using the Mac's own
60.15 Hz tick clock, 3 runs per measurement. MAME models the Plus's video bus contention. All
kernels were checked bit-exact against a Python reference (`verify.py`).

Reproduce: `python3 llm/bench/build_bench.py`, then run `llm/build/bench_disk.img` in MAME with
`-ramsize 4M -autoboot_script llm/bench/bench.lua` and `BENCH_OUT=<dir>`, then
`python3 llm/bench/verify.py llm/build/layout.json <dir>`.

## Seconds per token

| Ternary storage | Full model size | Memory 16 tokens | 64 tokens | 128 tokens |
| --- | --- | --- | --- | --- |
| **W4** (groups of 4, 16-bit codes) | 3.73 MB | **2.74** | **3.01** | 3.39 |
| W5 (groups of 5, 16-bit codes) | 3.12 MB | 2.90 | 3.18 | 3.55 |
| B4 (groups of 4, byte codes) | 2.14 MB | 3.12 | 3.40 | 3.77 |
| B5 (groups of 5, byte codes) | 1.85 MB | 3.40 | 3.68 | 4.05 |

All four full-size models fit in a 4 MB Plus (3,924K available to a program). Under the SE's
System 7.0.1 (2,267K largest free block) only B4 and B5 fit.

## Breakdown (W4, 64-token memory)

| Part | s/token |
| --- | --- |
| Ternary layers (incl. building lookup tables) | 1.78 |
| Attention (16 / 64 / 128-token memory) | 0.29 / 0.57 / 0.94 |
| 4-bit vocabulary step (2,048 tokens) + pick | 0.52 |
| Norms, quantization, SwiGLU | 0.14 |
| **Measured full token** | **3.03** (sum of parts 3.02) |

## Notes for the real implementation

- Accumulators are 16-bit. Real activations must be scaled so sums can't overflow, or use 32-bit
  table entries (`add.l`, a few cycles slower per lookup).
- Easy further speedups: a 1,024-token vocabulary halves the 0.52 s step (more tokens per reply);
  a shorter memory window cuts attention.
- Confirm on real hardware (the SE has the same 68000 at 7.83 MHz).

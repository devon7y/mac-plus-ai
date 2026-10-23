# Doodle "Finish it" decoder benchmark: results (2026-10-15)

One full decoder step of the class-conditional sketch-rnn decoder (recurrent cell, output layer
H → 123, sampling of the next pen move with temperature 0.5) was timed on MAME's Mac Plus
(ROM v3) with **1 MB**, on the real shapes, using the Mac's own 60.15 Hz tick counter. Each
number is one measurement of at least about 2 s emulated (120–150 ticks, so ±1%); two full runs
agreed within 1%. Every configuration was checked **bit-exact** against the Python reference
(`ref.py`): category fold, recurrent sums, inputs, gates, new state, output layer, sample, and
an 8-step trace that feeds each sampled point back in (12–13 checks per configuration, all 17
configurations OK). A deliberately corrupted dump is caught (negative test).

The integer formats are in [`FORMAT.md`](FORMAT.md) (the export contract for `paint/train/`).

Reproduce:

```sh
python3 paint/bench/build_bench.py                       # -> paint/build/bench_disk.img
mkdir -p paint/build/run && BENCH_OUT=$PWD/paint/build/run \
  emu/run_test.sh paint/build/bench_disk.img paint/bench/bench.lua 1600 > paint/build/run/log.txt
python3 paint/bench/verify.py paint/build/layout.json paint/build/run   # ALL KERNELS BIT-EXACT
python3 paint/bench/report.py paint/build/layout.json paint/build/run/log.txt
```

(About 410 s emulated, under 4 minutes wall.)

## Seconds per pen move (GRU, measured)

| Hidden size | **W4** ternary, 16-bit codes | B4 ternary, byte codes | P4 4-bit pairs |
| --- | --- | --- | --- |
| 192 | **0.147** | 0.181 | 0.274 |
| 256 | **0.229** | 0.290 | 0.432 |
| 384 | **0.446** | 0.574 | 0.859 |

A pen move = recurrence + output layer + sampling. Drawing the segment with QuickDraw adds
about a millisecond (not measured here).

LSTM, for comparison (same storage, measured): 0.187 / 0.295 / 0.578 s (W4, H = 192/256/384),
0.233 / 0.371 / 0.743 s (B4), 0.341 / 0.549 s (P4, 192/256). An LSTM step costs about 1.3 times a
GRU step of the same width, so the GRU is the better use of time.

## Full "Finish it" (prime 50 user points + generate 100), GRU

`fold` (category embedding, once) + 50 × priming step (no output layer, no sampling) + 100 × pen move:

| H | W4 | B4 | P4 |
| --- | --- | --- | --- |
| 192 | 21 s | 26 s | 39 s |
| 256 | **33 s** | 42 s | 63 s |
| 384 | **65 s** | 84 s | 125 s |

The initial state `h0 = tanh(W0 e + b0)` (training's design) adds H more rows to the fold:
about +0.08 / +0.11 s once per drawing (H = 256 / 384). Not included above.

## Breakdown (seconds per step, measured part by part)

| Part | 192 W4 | 256 W4 | **384 W4** | 256 P4 |
| --- | --- | --- | --- | --- |
| Lookup tables from the new hidden state | 0.007 | 0.009 | 0.014 | 0.053 |
| Recurrent matrix (3H × H) | 0.082 | 0.145 | 0.321 | 0.285 |
| Inputs (dx, dy, pen) | 0.015 | 0.020 | 0.028 | 0.019 |
| Gates and state update | 0.022 | 0.029 | 0.043 | 0.029 |
| Output layer (123 × H) | 0.020 | 0.026 | 0.037 | 0.048 |
| Sampling (mixture pick, Box–Muller, pen) | 0.001 | 0.001 | 0.001 | 0.001 |
| **Measured pen move** (sum of parts) | **0.147** (0.147) | **0.229** (0.230) | **0.446** (0.444) | **0.432** (0.435) |
| Priming step | 0.125 | 0.203 | 0.409 | 0.385 |
| Category fold, once per drawing | 0.173 | 0.229 | 0.340 | 0.228 |

Throughput of the recurrent matrix: W4 about 1.36M weights/s (H = 256), B4 1.01M, P4 0.69M.
Folding the category embedding into a per-drawing bias matters: done every step it would add
the fold time (0.23 s at H = 256) to each pen move.

## Memory (1 MB Plus, trimmed System 6, measured)

- `BufPtr − ApplZone` = **873,022 bytes (853K)**, which confirms the 852K figure. After
  `_MaxApplZone`, with the 29.6 KB benchmark code loaded: FreeMem 833,790, **MaxMem 827,930**.
- Runtime footprint of each model (weights in RAM format, lookup tables, per-row constants,
  345 × 32 embedding table, state, fixed-point tables), each allocated with `_NewPtr` on the
  1 MB machine:

| H | W4 | B4 | P4 | (LSTM W4 / B4 / P4) |
| --- | --- | --- | --- | --- |
| 192 | 131K fits | 98K fits | 237K fits | 159K / 117K / 283K fit |
| 256 | 190K fits | 134K fits | 355K fits | 236K / 164K / 433K fit |
| 384 | **345K fits** | 225K fits | 665K fits | 437K / 282K / **829K does not fit** |

With the paint app (code, 52K MacPaint page, 52K undo copy, menus: estimated 200–250K), a
model up to about 550K leaves room. GRU 384 P4 (665K) would not.
The app's model adds about 25K to these figures (the initial-state rows `W0` and a 16-bit
embedding table; see FORMAT.md). On disk, ternary weights pack 5 per byte (GRU 384: 98K for
both matrices) and expand to W4 codes at load time; the whole model file is about 170–190K.

## Choice (made by the paint agent on the manager's instruction; the manager may override)

**GRU, ternary weights, W4 storage, H = 384**: 0.45 s per pen move, about 65 s for a
50-point prime plus 100 new points, 345K of RAM. It is the largest model within the 0.5 s
target, and W4 is the fastest storage at every size (B4 saves RAM that isn't needed;
4-bit pairs cost twice the time and force a coarser hidden state, A ≈ 50 instead of 127).

**Fallback: H = 256 (same storage), twice as fast (0.23 s, 33 s per Finish-it, 190K).** Use it
if the trained ternary H = 256 model's drawings look as good as H = 384's (validation loss
within a few hundredths of a nat per move). 4-bit H = 256 (0.43 s) is the only other option
within 0.5 s; take it only if `int4` H = 256 clearly beats ternary H = 384 in validation.
The app reads H and the storage kind from the model file, so this choice can change when
the trained models arrive without changing code.

## Notes for the real implementation

- Sums are 16-bit with wraparound; exactness needs `Σ|w|·A ≤ 32767` per row (the exporter
  picks A; see FORMAT.md).
- Easy speedups if needed: inline the row loop (about 2–3%), skip the output layer during
  priming (already done in the priming step).
- **Correction (2026-10-16):** MAME does **not** model the Plus's video/sound bus contention (its
  contention code runs where it has no effect; see toolchain/README.md, "Real-hardware speed"). A real
  Plus runs RAM-bound code about 1.5× slower, an SE about 1.3×. All times in this file are MAME
  times; multiply by about 1.5 for a real Plus, or measure with `SLOW=1.5` (see below).

## In the app (measured, 2026-10-16)

The same kernels run inside Doodle (`paint/app`, generated by `gen_kernels.py` from
`bench/kernels.py`). With an H = 384 ternary model (random weights; W4 in RAM), MAME Mac Plus 1 MB,
from the M6 floppy (`paint/app/tests/test_finish.lua` + `check_m5.py`):

| What | Measured |
| --- | --- |
| Model RAM (codes expanded at load) | 383,568 bytes; FreeMem afterwards 258,508 |
| Loading "Doodle Model" (217,716 bytes) from the floppy, first Finish it only | 10.9 s |
| Priming (fold + 11 steps + 1 full step) | 5.64 s for 12 inputs (0.47 s each) |
| Drawing | **0.464 s per pen move** (200 moves in 92.8 s), drawing included |
| Same floppy on the emulated SE (4 MB) | 0.463 s per move |

All 200 generated moves of every run matched `ref.finish_it` exactly (same prompt, category,
seed and temperature).

Under **System 7.0.1 on the SE** (4 MB, hard disk, launched by the Finder from Startup Items;
`paint/app/tests/run_se7.sh`): 0.465 s per move, model loaded from SCSI in 2.5 s, FreeMem 514K
after loading inside Doodle's 1,100K partition (SIZE resource: 1,100K preferred, 800K minimum).


## Shipped model: 4-bit (P4) H = 256 (2026-10-16)

`h256_int4_ft.pt` exported with A forced to 63 (`export.py ckpt ... --A 63`); P4 sums now use two
16-bit accumulators per row combined with saturation (`mv_P`). Measured in Doodle, Finish it from
the floppy (`run_final.sh` and `test_finish.lua`):

| Machine | Seconds per pen move | Priming, per input | Model load |
| --- | --- | --- | --- |
| MAME Plus 1 MB (full 7.83 MHz) | 0.434–0.441 | 0.45–0.47 | 11.5 s (floppy) |
| **MAME Plus, `SLOW=1.5` (real-Plus estimate)** | **0.66** | 0.73 | 11.4 s |
| MAME SE 4 MB, `SLOW=1.3` (real-SE estimate) | 0.57 | 0.63 | 12.6 s |
| MAME SE, System 7.0.1, hard disk | 0.435–0.441 | 0.47 | 2.5 s |

Model RAM 372,624 bytes; FreeMem afterwards 267K on the 1 MB Plus. A typical real-Plus Finish it:
priming 50 points about 30 s, then about 0.66 s per new move (completions median 16 moves, so ~10 s;
drawings from scratch median 34 moves).

Saturating P4 sums cost +1.4% (benchmark GRU 256 P4 0.436 → 0.442 s per move in MAME).

**Final model `h256_int4_300k` (same format, A = 63):** MAME 0.435–0.440 s per move on the Plus, SE and
SE/System 7; **`SLOW=1.5` (real Plus) 0.66 s per move**, 0.73 s per primed input; bit-exact everywhere.

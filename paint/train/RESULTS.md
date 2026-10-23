# Doodle "Finish it": training results (M3, 2026-10-16)

Class-conditional sketch-rnn decoders trained on all 345 *Quick, Draw!* categories (CC BY 4.0,
Google; credit it in the app's About box) on Rorqual H100s: hidden sizes 192, 256 and 384, a
full-precision reference for each, and quantization-aware training (QAT) matching
`paint/bench/FORMAT.md`. Everything here was run and measured unless marked *estimated*.

## Summary

- **Shipped model (manager's decision, 2026-10-16 01:15): `checkpoints/h256_int4_ft.pt`**.
  GRU H=256 with 4-bit weights (P4) in the recurrent and output matrices, one scale per row,
  QAT fine-tuned for 100K steps from the full-precision H=256 model. **Validation loss 0.8280**
  (full validation set, 862,492 drawings), test 0.8289. The Mac runs it at **A=63**: 0.8466 in
  the Mac-arithmetic simulation (float on the same drawings 0.8423), largest 16-bit sums
  7,884 (recurrent) and 10,499 (output), no overflow. 0.432 s per pen move (benchmark).
- **Better candidate, same format: `checkpoints/h256_int4_300k.pt`** (300K fine-tuning steps
  instead of 100K): **validation 0.8183**, test 0.8194, Mac simulation at A=63 **0.8403**
  (largest sums 9,746 / 10,849, no overflow). Handed to the manager at 01:55 for the app's
  `run_final.sh`.
- Previous shipped model, `checkpoints/h384_tern_r2o.pt`: GRU H=384, recurrent matrix ternary
  W4 with one scale per row, output layer as **two ternary terms** (`Wo = s1*T1 + s2*T2`, both
  W4). Validation loss 0.8668, test 0.8677, Mac simulation 0.8864 at A=85, about 0.48 s per move.
  The full-precision H=384 twin reaches 0.6645; plain ternary 0.9303 (`h384_tern_ft.pt`, the
  first interim model).
- **Sampling matters as much as the weights.** Temperature 0.5 applied to the pen-state softmax
  squares the odds against lifting the pen, so strokes run on and curl into "scribble balls"
  (mean 83 moves per drawing against 39.6 in the data). Keeping the pen state at temperature
  1.0 while the mixture uses 0.3-0.5 removes almost all of them (47-50 moves, none hit the cap)
  and gives the most recognizable drawings of any setting. The price: completions end sooner
  (28 vs 49 moves), because the end-of-drawing state shares that softmax.
- **Quality, honestly:** at mixture 0.3-0.5 with pen 1.0, both the P4 H=256 and the
  two-term ternary H=384 model draw recognizable Quick, Draw!-style doodles: bicycles with two
  wheels and a frame, cars and trucks with wheels, smiley faces, cloud-canopy trees, flowers on
  stems, Eiffel towers, houses, cats with ears and whiskers. P4 H=256 looks equal or slightly
  better. Both are clearly below the full-precision models, and completions are hit-and-miss
  (wheels added to a car body almost always; birds and dogs often wrong).
- **Why P4 H=256 won:** it is about 0.04 better than two-term ternary H=384 in loss and about
  10% faster per move (0.432 s vs about 0.48 s). It only works because the Mac runs it at A=63
  instead of FORMAT's worst-case A=32 (0.8622): over 6,900 drawings no sum came near 16 bits
  even at A=127 (largest 21,117), and the app adds a saturating combine as insurance.

## Results

Loss per point (lower is better; offsets negative log-likelihood + pen cross-entropy).
Val and test: full sets (862,492 / 862,487 drawings). "Mac sim": the Mac's arithmetic
simulated on 6,900 validation drawings (see "Mac arithmetic check") at the A the app uses.
Seconds per move: `paint/bench` and the app's measurements (1 MB Plus, emulated).

| Model (checkpoint) | Format | s per move | Val | Test | Mac sim (A) |
| --- | --- | --- | --- | --- | --- |
| **`h256_int4_300k`** (best) | GRU 256, 4-bit Wh and Wo | 0.432 | **0.8183** | 0.8194 | **0.8403** (63) |
| `h256_int4_ft` (shipped 01:15) | same, 100K fine-tuning steps | 0.432 | 0.8280 | 0.8289 | 0.8466 (63) |
| `h384_tern_r2o` | GRU 384, ternary Wh, two-term ternary Wo | 0.48 | 0.8668 | 0.8677 | 0.8864 (85) |
| `h384_tern_ft` (first interim) | GRU 384, ternary Wh and Wo | 0.446 | 0.9303 | 0.9311 | 0.9510 (85) |
| fp H=384 (reference) | float | | 0.6645 | 0.6656 | |
| fp H=256 (reference) | float | | 0.7400 | 0.7409 | |
| fp H=192 (reference) | float | | 0.7891 | 0.7901 | |

All runs (`collect.py`; `_ft` = QAT fine-tuned from the fp model, `_v2` = QAT from scratch,
`_kd`/`_kdh` = distillation; "Mac sim, A_auto" uses FORMAT.md's worst-case A; the overflow
columns count 16-bit sums beyond +-32,767 at A=127; "Moves/sample" = mean length of the
temperature-0.5 grid samples, against 39.6 for real drawings):

| Run | H | Weights | Steps | Val loss | (offsets + pen) | Test loss | Subset: float | Mac sim, A_auto | A_auto | Mac sim A=127 wrapped | Overflow rec/out at A=127 | Moves/sample |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| h192_fp | 192 | fp | 300000 | 0.7891 | 0.491 + 0.298 | 0.7901 | 0.8057 |  |  |  |  /  | 76.3 |
| h192_tern_ft | 192 | tern | 100000 | 1.0724 | 0.751 + 0.321 | 1.0735 | 1.0883 | 1.0899 | 127 | 1.0899 | 0.00000 / 0.00000 | 70.1 |
| h192_tern_o4_ft | 192 | tern_o4 | 100000 | 0.9663 | 0.652 + 0.315 | 0.9674 | 0.9838 | 0.9933 | 48 | 0.9849 | 0.00000 / 0.00000 | 84.5 |
| h192_int4_ft | 192 | int4 | 100000 | 0.8735 | 0.569 + 0.305 | 0.8746 | 0.8895 | 0.9055 | 42 | 0.8913 | 0.00000 / 0.00000 | 64.5 |
| h192_int4_v2 | 192 | int4 | 300000 | 0.8895 | 0.585 + 0.305 | 0.8904 | 0.9052 | 0.9124 | 39 | 0.9060 | 0.00000 / 0.00000 | 80.7 |
| h256_fp | 256 | fp | 300000 | 0.7400 | 0.447 + 0.293 | 0.7409 | 0.7552 |  |  |  |  /  | 68.8 |
| h256_tern_ft | 256 | tern | 100000 | 1.0139 | 0.700 + 0.314 | 1.0149 | 1.0301 | 1.0317 | 127 | 1.0317 | 0.00000 / 0.00000 | 62.8 |
| h256_tern_ft400k | 256 | tern | 400000 | 1.0121 | 0.698 + 0.314 | 1.0130 | 1.0278 | 1.0292 | 127 | 1.0292 | 0.00000 / 0.00000 | 62.2 |
| h256_tern_kdh | 256 | tern | 300000 | 1.0540 | 0.735 + 0.319 | 1.0549 | 1.0686 | 1.0694 | 127 | 1.0694 | 0.00000 / 0.00000 | 69.0 |
| h256_tern_r2o_ft | 256 | tern_r2o | 100000 | 0.9443 | 0.634 + 0.310 | 0.9452 | 0.9605 | 0.9618 | 127 | 0.9618 | 0.00000 / 0.00000 | 80.4 |
| h256_tern_r2o_ft300k | 256 | tern_r2o | 300000 | 0.9460 | 0.635 + 0.311 | 0.9469 | 0.9608 | 0.9623 | 127 | 0.9623 | 0.00000 / 0.00000 | 79.7 |
| h256_tern_o4_ft | 256 | tern_o4 | 100000 | 0.9201 | 0.611 + 0.309 | 0.9211 | 0.9353 | 0.9567 | 35 | 0.9377 | 0.00000 / 0.00000 | 77.1 |
| h256_int4_ft | 256 | int4 | 100000 | 0.8280 | 0.529 + 0.299 | 0.8289 | 0.8423 | 0.8622 | 32 | 0.8429 | 0.00000 / 0.00000 | 60.6 |
| h256_int4_ft300k | 256 | int4 | 300000 | 0.8183 | 0.521 + 0.298 | 0.8194 | 0.8340 | 0.8569 | 31 | 0.8361 | 0.00000 / 0.00000 | 73.8 |
| h256_int4_v2 | 256 | int4 | 300000 | 0.8317 | 0.532 + 0.300 | 0.8329 | 0.8475 | 0.8593 | 31 | 0.8480 | 0.00000 / 0.00000 | 86.1 |
| h384_fp | 384 | fp | 300000 | 0.6645 | 0.379 + 0.286 | 0.6656 | 0.6798 |  |  |  |  /  | 61.0 |
| h384_tern_ft | 384 | tern | 100000 | 0.9303 | 0.625 + 0.306 | 0.9311 | 0.9463 | 0.9510 | 85 | 0.9487 | 0.00000 / 0.00000 | 65.2 |
| h384_tern_ft400k | 384 | tern | 400000 | 0.9284 | 0.623 + 0.305 | 0.9292 | 0.9444 | 0.9478 | 85 | 0.9459 | 0.00000 / 0.00000 | 64.2 |
| h384_tern_kd | 384 | tern | 300000 | 0.9580 | 0.648 + 0.310 | 0.9589 | 0.9748 | 0.9787 | 85 | 0.9766 | 0.00000 / 0.00000 | 77.5 |
| h384_tern_kdh | 384 | tern | 300000 | 0.9711 | 0.661 + 0.310 | 0.9719 | 0.9877 | 0.9924 | 85 | 0.9896 | 0.00000 / 0.00000 | 75.9 |
| h384_tern_g128_ft | 384 | tern_g128 | 100000 | 0.9207 | 0.616 + 0.304 | 0.9218 | 0.9362 | 0.9409 | 85 | 0.9385 | 0.00000 / 0.00000 | 67.6 |
| h384_tern_g64_ft | 384 | tern_g64 | 100000 | 0.9140 | 0.610 + 0.304 | 0.9149 | 0.9291 | 0.9338 | 85 | 0.9313 | 0.00000 / 0.00000 | 71.5 |
| h384_tern_r2o_ft | 384 | tern_r2o | 100000 | 0.8668 | 0.564 + 0.302 | 0.8677 | 0.8816 | 0.8864 | 85 | 0.8839 | 0.00000 / 0.00000 | 82.6 |
| h384_tern_r2o_ft300k | 384 | tern_r2o | 300000 | 0.8662 | 0.563 + 0.303 | 0.8672 | 0.8821 | 0.8857 | 85 | 0.8838 | 0.00000 / 0.00000 | 84.1 |
| h384_tern_o4_ft | 384 | tern_o4 | 100000 | 0.8457 | 0.545 + 0.301 | 0.8468 | 0.8601 | 0.9254 | 26 | 0.8634 | 0.00000 / 0.00000 | 74.0 |
| h384_int4_ft | 384 | int4 | 100000 | 0.7446 | 0.454 + 0.291 | 0.7455 | 0.7587 | 0.8565 | 23 | 0.7623 | 0.00000 / 0.00000 | 59.0 |
| h384_int4_v2 | 384 | int4 | 300000 | 0.7533 | 0.461 + 0.293 | 0.7545 | 0.7689 | 0.8371 | 19 | 0.7711 | 0.00000 / 0.00000 | 78.8 |

Ternary-from-scratch runs (`h*_tern_v2`) were stopped at their plateau and not evaluated:
validation subset 1.199 (H=384, 192.5K steps), 1.255 (H=256, 197.5K), 1.311 (H=192, 117.5K).

## What worked and what didn't

1. **Plain ternary QAT trained from scratch fails.** A first version (gradient passed straight
   through the whole quantizer) diverged to NaN in all six QAT runs after 38-57K steps: latent
   weights grew without bound (ternary gradient norms climbed from about 1 to 3e7). With the
   fix (STE on the rounding only, clamp and scale in the graph, skip non-finite updates),
   from-scratch ternary plateaus at 1.20-1.31 (H=384/256/192). 4-bit from scratch works
   (0.753 / 0.832 / 0.890), a little worse than fine-tuned 4-bit (0.745 / 0.828 / 0.874).
2. **Fine-tuning QAT from the full-precision model** is the way: ternary H=384 reaches 0.930
   in 100K steps (from-scratch: 1.20). Longer fine-tuning only helps 4-bit (H=256: 0.828 at
   100K, 0.818 at 300K); ternary (0.928 at 400K) and two-term ternary (0.866 at 300K) have
   plateaued by 100K.
3. **The ternary output layer costs about 0.09 nats.** Replacing it with 4-bit (`tern_o4`)
   gives 0.846 (H=384), but a 4-bit Wo forces FORMAT's A down to 26, which loses most of the
   gain on the Mac (0.925 simulated). A **second ternary term** for Wo (`tern_r2o`) keeps W4
   and A: 0.867, for +0.035 s per move (measured by the app agent: 0.476-0.483 s against
   0.445-0.464 s).
4. **Per-group scales** inside each Wh row (simulation only): groups of 64 give 0.914 and
   groups of 128 give 0.921, against 0.930 per row. Small gain; not pursued.
5. **Distillation** from the full-precision teacher (KL on mixture weights and pen states plus
   teacher-weighted component matching, optionally hidden-state MSE), started from the
   ternary model: worse on data likelihood (0.958 with output terms only, 0.971 with the
   hidden-state term, against 0.930 before distilling), as the
   student is pulled toward outputs it can't represent. Not pursued.
6. **FORMAT.md's A is conservative.** `A = min(127, 32767 / max row L1)` guarantees no
   overflow, but over 6,900 validation drawings the largest sums at A=127 were far below
   32,767 for every model (ternary H=384: about 4-5K at A=85; 4-bit H=256: up to 19.6K
   recurrent / 21.8K output).
   For 4-bit models the worst-case A costs 0.02-0.09 nats.

## Data (verified)

- Source: `https://storage.googleapis.com/quickdraw_dataset/sketchrnn/<category>.npz`
  (spaces URL-encoded as `%20`), the 345 categories of the official `categories.txt`. Every
  category has exactly 70,000 / 2,500 / 2,500 train / valid / test drawings (24,150,000 /
  862,500 / 862,500 in total); 1.8 GB.
- These files are **not** in the 0-255 box: they are raw screen pixels, RDP-simplified at
  that scale (median drawing about 340-520 px across). `prep_data.py` converts every drawing
  to the Mac's 256x256 AI frame the way the dataset's "simplified" files are made: absolute
  points, shift to 0, scale so the larger side is 255, round to whole pixels, drop repeated
  points, **RDP with epsilon 2.0 per stroke** (`rdp()` in `prep_data.py` is the reference
  the Mac must copy for the user's strokes), drop single-point strokes (they draw nothing in
  stroke-3), and drop drawings with more than 250 moves. Drawings start at their first point
  (the source's first row, an offset from an arbitrary origin, is discarded).
- Kept: 24,149,761 / 862,492 / 862,487 drawings (239 / 8 / 13 dropped, mostly `camouflage`,
  `blackberry`, `line`), 955M training moves, **mean 39.6 moves per drawing** (p99 108).
  Global scale factor **S = 35.208** px (std of dx and dy over the training set): the network
  sees dx/S, dy/S.

## Model (`sketch_model.py`)

- Input per step: stroke-5 `(dx/S, dy/S, p_down, p_up, p_end)`; the first input is
  `(0, 0, 1, 0, 0)`; moves are relative to the drawing's first point.
- GRU in PyTorch/cuDNN form (`n = tanh(Wx x + Wc e + b_in + r * (Wh h + b_hn))`). The category
  embedding `e` (345 x 32) enters through `Wc` (3H x 32), constant per drawing, so the Mac
  folds `Wc e + b` once per drawing.
- Initial state `h0 = tanh(W0 e + b0)` (once per drawing).
- Output 123 = 20-component bivariate Gaussian mixture + 3 pen logits, in the order pi, mu_x,
  mu_y, log sigma_x, log sigma_y, rho (pre-tanh), pen. FORMAT.md uses this order.
- Loss per point = offset negative log-likelihood (normalised units; targets dequantized with
  uniform +-0.5 px noise so the density can't collapse onto whole pixels) over real moves +
  pen cross-entropy over the moves and the end token, averaged over points. Not comparable with
  the sketch-rnn paper's L_R, which divides by N_max = 250 and counts padding.
- Parameters: 174K (H=192), 278K (H=256), 558K (H=384).

## Quantization-aware training (matches `paint/bench/FORMAT.md`)

Integer codes times a float scale per output row (the Mac's per-row multiplier):

| Matrix | Mac work | `tern` | `tern_r2o` | `int4` (shipped, H=256) |
| --- | --- | --- | --- | --- |
| Wh (3H x H), recurrent | every step (85% of MACs at H=256) | ternary, scale = mean abs | ternary | codes -7..7, scale = max abs / 7 |
| Wo (123 x H), output | every step | ternary | s1*T1 + s2*T2, T2 = ternary of the residual | 4-bit |
| Wx (3H x 5), dx/dy/pen | every step, tiny | int8 (16-bit on the Mac) | int8 | int8 |
| Wc, W0 (fold, h0) | once per drawing | 4-bit | 4-bit | 4-bit |
| Embedding (345 x 32) | once per drawing | int8 | int8 | int8 |
| Biases | | float (Q10 on the Mac) | float | float |

Fake quantization: straight-through estimator on the rounding only; the clamp and the scale
stay in the graph. `export_codes()` produces codes and scales; `evaluate.py` writes them to
`export.npz` (`wo_codes` is `[2, 123, H]` and `wo_scale` `[2, 123]` for `tern_r2o`).
Other variants tried: `tern_o4` (ternary Wh, 4-bit Wo), `tern_g64` / `tern_g128` (ternary Wh
with one scale per group of 64 / 128 columns).

## Training

Adam, batch 1,024 drawings (length-bucketed, all data resident on the GPU), cosine learning
rate with warmup, gradient clipping 1.0, random x/y stretch +-10%, validation on a fixed
34,500-drawing subset (100 per category) every 2,500 steps; the table's val/test numbers are
on the full sets. One H100 does 130-147 steps/s at H = 256/384 (5.5M moves/s; one pass over
all 24M drawings takes about 3 minutes) and about 75 steps/s at H = 192.

- fp: 300K steps (12.7 epochs), lr 2e-3, about 40 min (H = 256/384).
- QAT from scratch (`_v2`): 300K steps, lr 1e-3.
- QAT fine-tuned from fp (`_ft`): 100K steps, lr 5e-4, warmup 500 (`_ft300k`, `_ft400k`:
  longer). Distillation (`_kd`, `_kdh`): 300K steps from `h384_tern_ft`.

## Mac arithmetic check

`evaluate.py: mac_loss()` recomputes the loss through the Mac's pipeline (FORMAT.md) in float
on 6,900 validation drawings (20 per category): the hidden state stays full precision but is
quantized to `hq = floor(h*A)` for each matrix product; the products are computed exactly on
the integer codes (sums beyond 16 bits are counted, then wrapped); per-row scales; sigmoid
and tanh inputs clamped and binned at the table resolution (1/64, 1/128); outputs clamped to
the Q10 range. `A_auto` is FORMAT.md's `min(127, floor(32767 / max row L1))`. It doesn't model
the 16-bit rounding of the input and bias terms or the fixed-point multipliers (M4's
bit-exact reference covers those; the app agent measured float vs integer outputs within
0.002). It also checks the step-by-step cell against cuDNN (identical losses).

## Samples

For each run in `samples/<run>/` (same seeds for every model):

- `grid_a.png`, `grid_b.png`: 20 categories x 6 drawings at temperature 0.5 (cat, bicycle,
  Eiffel Tower, house, tree, face, car, fish, flower, airplane, apple, butterfly, clock, cup,
  guitar, pig, rabbit, sailboat, snowman, umbrella).
- `grid_*_t0.3.png`: temperature 0.3. `*_pen1.png` / `*_pen0.75.png`: the pen state sampled at
  temperature 1.0 / 0.75 while the mixture keeps 0.5 (or 0.3).
- `complete_a.png`, `complete_b.png` (and `_pen1`, `_pen0.75`): 20 held-out test drawings, one
  per category with at least 4 strokes: original in grey, the prompt (first 40% of its strokes),
  then 4 completions (prompt black, model red).

Each cell shows the 256x256 AI frame (grey square) at half scale. Sampled moves are rounded to
whole pixels and fed back rounded, as on the Mac. "Leaving the frame" counts in `metrics.json`
are strict (originals span the full 255 px, so a 1-px overshoot counts); the app should clip
to the frame.

## Files

- `download.sh` (fetches the 345 npz files; run on a Rorqual login node), `prep_data.py` (data conversion; the RDP reference), `sketch_model.py` (model, quantizers,
  loss, distillation, GPU batching), `train.py`, `evaluate.py` (losses, Mac simulation,
  sampling, PNGs, integer export), `collect.py` (the table), `slurm/` (job scripts as run).
- `checkpoints/<name>.pt`: float latent weights (`torch.load(p)["model"]`, `["args"]`,
  `["scale"]`); load with `SketchDecoder(H, quant)`. `checkpoints/<name>_export.npz`: integer
  codes, per-row scales, float biases, `scale_factor`.
- On Rorqual (`/scratch/devon7y/macplus_paint/`): `data/` (npz and prepared arrays, 6 GB),
  `runs/` (all checkpoints and `log.jsonl` training curves), `eval/`, `logs/`, `runs_failed/`
  (the diverged first QAT attempt).

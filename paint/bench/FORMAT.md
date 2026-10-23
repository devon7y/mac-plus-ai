# Doodle decoder: integer formats on the Mac (the export contract)

This is how the Mac computes one decoder step, exactly. `paint/bench/ref.py` is the bit-exact
Python version; the benchmark (`gen_bench.py`) and the app use the same arithmetic. The
trained PyTorch model (`paint/train/sketch_model.py`) maps onto it as described in
"Exporting a trained model". Tables are in `paint/bench/tables.py`.

Notation: `Qn` = signed integer holding value × 2^n. `>>` = arithmetic shift right (floor).
`clip16` = clamp to [-32768, 32767]. All products are 16 × 16 → 32 bits (68000 `muls.w`).

## Model shape

- GRU, PyTorch form, hidden size `H` (192, 256 or 384). Recurrent matrix `U` has 3H rows in the
  order **r, z, n**, H columns.
- Input per step: stroke-5 point `(dx, dy, p1, p2, p3)` with **dx, dy integer pixels**
  (the network's float input is dx/S, dy/S; S = `scale_factor` from `meta.json`), plus the
  category embedding (32), whose contribution is constant per drawing (folded once).
- Output layer `WY`: 123 rows × H, in the training order **pi[0,20), mu_x[20,40), mu_y[40,60),
  log sigma_x[60,80), log sigma_y[80,100), rho (pre-tanh)[100,120), pen logits[120,123)**.

## Weight storage (U and WY)

The weights are ternary (−1/0/+1) or 4-bit (−8..7) integer codes with a per-row float scale
`s_row` (that is what `export_codes` returns). In RAM:

| Kind | Weights | RAM code | Lookup tables (built per step from `hq`) |
| --- | --- | --- | --- |
| **W4** | ternary | one 16-bit code per group of 4 columns: `(g*81 + idx)*2`, `idx = Σ_j (w_j+1)·3^j` | 81 entries per group, contiguous |
| B4 | ternary | one byte per group of 4: `idx*2` | same tables |
| P4 | 4-bit | one 16-bit code per pair of columns: `(p'*256 + c)*2 − 32768`, `c = (w1+8)*16 + (w2+8)` | 256 entries per pair; pairs ≥ 128 use a second 64 KB window and come first in each row |

`decode_w4/decode_b4/decode_p4` in `ref.py` define these exactly.

**P4 rows use two accumulators** (`mv_P`, `ref.matvec_p4`): the last 64 pairs of each row in kernel
order (pairs ≥ 128 first, then 0..127) go into a second 16-bit register; the two wrapped 16-bit
partial sums are sign-extended, added in 32 bits and clamped to int16:
`S = clip16(wrap16(Σ_A) + wrap16(Σ_B))`. A sum beyond int16 then saturates instead of wrapping
(exact as long as each half fits). This lets a 4-bit model use a larger A than the worst-case bound
(the shipped H = 256 model: A = 63, its largest observed sums about a third of int16). On disk the ternary weights
can be packed 5 per byte (or B4) and expanded to W4 codes at load time.

## Hidden state and the matrix-vector product

- `h` (H values) is kept in **Q14**.
- Before each matrix product it is quantized: `hq = (h * A) >> 14`, `A ≤ 127` a model constant.
- `S = U · hq` is accumulated in **16 bits with wraparound**. The result is exact only if every
  row satisfies `Σ_j |w_ij| · A ≤ 32767`, so the exporter chooses
  `A = min(127, floor(32767 / max_row_L1))` over the rows of U **and** WY (they share `hq`).
  Ternary at H ≤ 256 always allows A = 127. 4-bit weights typically give A ≈ 40–60, which is
  coarser hidden-state resolution; QAT/eval should simulate `hq = floor(h·A)/A`.

## One GRU step

```
cx[i]   = ((WDX[i]*dx + WDY[i]*dy) >> SHX) + E[i] + P[pen][i]              (32-bit, Q10; SHX in the model file)
r,z rows (i < 2H):
  pre   = clip(cx[i] + ((S[i]*M[i]) >> SHR), -8192, 8191)                  Q10
  g[i]  = SIG[(pre >> 4) + 512]                                             Q14
n rows (j = i - 2H):
  u     = clip16(((S[i]*MN[j].m) >> SHR) + MN[j].bhn)                       Q10
  pre   = clip(cx[i] + ((r[j]*u) >> 14), -4096, 4095)
  n[j]  = TANH[(pre >> 3) + 512]                                            Q14
h'      = n + ((z*(h - n)) >> 14)                                           Q14
hq'     = (h'*A) >> 14
```

Output layer: `SY = WY · hq_o` (16-bit wraparound) with its own quantization `hq_o = (h'*AO) >> 14`
(equal to `hq'` when `AO = A`; the app then also skips the second set of lookup tables),
`O[k] = clip16(BY[k] + ((SY[k]*MY[k]) >> SHO))`, Q10. `WY` may use a different storage kind from
`U` (training's `tern_o4`: ternary `U`, 4-bit `WY`); then `AO` comes from `WY`'s rows.

(LSTM variant, benchmark only: rows i, f, o, g; `c` in Q12;
`c' = clip16(((f*c) >> 14) + ((i*g) >> 16))`, `h' = (o * TANH[(clip(c', ±16384) >> 5) + 512]) >> 14`.)

### Two-term ternary output layer (`tern_r2o`, model-file flag bit 0)

`WY ≈ s1·T1 + s2·T2` per row: `T1`, `s1` = ternary codes and absmean scale of `Wo`; `T2`, `s2` =
the same for the float32 residual `Wo − s1·T1` (exactly `sketch_model.export_codes(w, "tern_r2")`).
Both are stored like `WY` (W4 in RAM) and read the same `hq_o` lookup tables:

```
SY1 = T1 · hq_o,  SY2 = T2 · hq_o                       (16-bit wraparound each)
O   = clip16(BY + ((SY1*MY) >> SHO) + ((SY2*MY2) >> SHO2))   (products 32-bit, arithmetic
                                                             shifts, summed in 32 bits, one clip)
MY  = round(s1 · 2^(10+SHO) / AO),  MY2 = round(s2 · 2^(10+SHO2) / AO)   (each its largest shift)
BY  = round(1024·bo + 1024·0.5·(s1·Σ_j T1 + s2·Σ_j T2)/AO)
AO  = min(127, floor(32767 / max row L1 over T1 and T2))
```

Training needs no change for this: its fake quantization (`q_tern_r2`) defines the codes and
scales; the integer rounding here is below the activation quantization the Mac already adds.
Cost on the Mac (measured): +0.031–0.036 s per pen move at H = 384 (0.481 s), +24 KB of codes.
Old model files (flag 0) load unchanged.

## Sampling (temperature τ)

Scalars: `INVT = round(256/τ)` (Q8), `HLT = round(512·ln τ)` (Q10 of ½ ln τ), `INVTP = round(256/τ_pen)`
(the pen pick's own temperature), `FQ = round(64·S)`. Finish menu settings (app constants, same in
`ref.TEMP_SETTINGS`): Tidy τ 0.3, Normal **τ 0.5 (default)**, Wild τ 0.8, all with **pen τ 0.75**
(INVT 853 / 512 / 320, HLT −616 / −355 / −114, INVTP 341). With the shipped P4 H = 256 model, pen 0.75
gives no loops and the fewest early stops (pen 0.5 looped on the earlier ternary model; pen 1.0 stops
completions early more often).

```
softmax_pick(logits):  l = (q*INVT) >> 8;  p_j = EXPP[min((max(l) - l_j) >> 5, 511)]   (Q11)
                        t = ((rand & 0xFFFF) * Σp) >> 16;  pick first j with t < cumsum_j
k        = softmax_pick(O[0:20])                     (random number 1)
s1, s2   = EXPS[(clip(O[60+k] + HLT, -8192, 4095) >> 4) + 512], same for O[80+k]   (Q10; sigma*sqrt(tau))
rho, c   = TANH[i], SECH[i] with i = (clip(O[100+k], ±4096) >> 3) + 512             (Q14; c = sqrt(1-rho^2))
r        = rand (random number 2); R = RTAB[r >> 22] (Q12); a = (r >> 12) & 1023
n1       = (R*COS[a]) >> 14;  n2 = (R*COS[(a+768) & 1023]) >> 14                      (Q12, Box-Muller)
x        = O[20+k] + ((s1*n1) >> 12)
y        = O[40+k] + ((s2*(((rho*n1) >> 14) + ((c*n2) >> 14))) >> 12)                  (Q10, normalised units)
pen      = softmax_pick(O[120:123]) with INVTP        (random number 3)
dx, dy   = clip(((clip16(x)*FQ + 32768) >> 16), -255, 255), same for y                  (pixels)
```

`rand` is xorshift32: `x ^= x << 13; x ^= x >> 17; x ^= x << 5` (32-bit, logical shifts).
The sampled `(dx, dy, pen)` is the next step's input. Training clamps log sigma to [−8, 4],
as the EXPS table does; it uses `rho = 0.999·tanh` for stability, the Mac uses plain tanh.

## Per-drawing constants (the category "fold")

From the category embedding `e` (row `cat` of `EMB`), once per drawing:

```
acc[i]  = Σ_j WC[i][j] * EMB[cat][j]            (32-bit; WC = int codes, EMB = int16)
E[i]    = clip16(B0[i] + (((acc >> SHA) * MC[i]) >> SHB))          (Q10)
```

For 3H rows (`Wc`, biases `B0 = round(1024·(b_ih + b_hh))` for r,z rows and
`round(1024·b_in)` for n rows) and H more rows for the initial state
(`W0`, `B0 = round(1024·b0)`), whose result goes through `TANH` to give `h0` in Q14.
`SHA`, `SHB`, `MC` as in "Exporting" below. The benchmark timed a single-shift version with the same
cost (32 multiplies per row); the app (`paint/app/gen_kernels.py`, `p_fold2`) and `ref.fold2` use
the two-shift form above.

## Exporting a trained model (float → integers)

With `s` the per-row scale from `export_codes` and `A`, `SHR`, `SHO` as above:

- `A = min(127, floor(32767 / max_row Σ|code|))` over `U`'s rows; `AO` the same over `WY`'s rows.
- `M[i] = round(s_wh[i] · 2^(10+SHR) / A)` for the r, z rows; `MN[j] = (same for the n row,
  round(1024·b_hn[j]))`. Choose the largest `SHR` (≤ 24) with every `|M| ≤ 32767`.
- `MY[k] = round(s_wo[k] · 2^(10+SHO) / AO)`, `BY[k] = round(1024·bo[k])`, same rule for `SHO`.
- `hq = floor(h·A)` is low by 0.5 on average, so each row's bias gets `+round(1024·0.5·s·Σ_j code_ij / A)`
  (r, z rows: in `B0`; n rows: in `bhn`; output rows: in `BY`, with `AO`).
- `WDX[i] = round(Wx_fq[i,0] · 1024 · 2^SHX / S)`, `WDY` likewise, with the largest `SHX` (≤ 12) that
  keeps them in int16 (header offset 44; 0 means 6). The input step is
  `cx = ((WDX·dx + WDY·dy) >> SHX) + E + P[pen]`.
- `P[pen][i] = round(1024 · (Wx_fq[i, 2+pen] − mid_i))` with `mid_i` the midpoint of row i's three pen
  weights, and `mid_i` added to that row's fold bias `B0` (exact: one pen column is active per step;
  the trained H = 384 model's pen weights reach −48, beyond Q10's ±32).
- `EMB[c][j] = round(emb_fq[c,j] · KE)` with `KE = 2^n`, the largest such that `|EMB| ≤ 32767`;
  `WC` = the int4 codes of `Wc` / `W0`; `SHA` the smallest shift with `|acc >> SHA| ≤ 32767` over
  all categories; `MC[i] = round(s_c[i] · 1024 · 2^(SHA+SHB) / KE)` with the largest `SHB` keeping
  `|MC| ≤ 32767`.

`paint/bench/export.py ckpt <checkpoint.pt> <out.dmdl>` does all of this and writes the model
file (`model_file.py`: a 64-byte header, the category names, then the integer arrays; the Mac
expands the packed codes at load time). `check_export.py` runs the training model's own float
step next to `ref.py` on the same drawing: for random ternary models the 123 outputs agree to
about 0.002 (correlation 0.9999).

**4-bit weights and the hidden-state scale.** Exact 16-bit sums need `Σ|code|·A ≤ 32767`. Ternary
rows give `A` ≈ 100–127; 4-bit rows (codes up to 8) give `A` = 32 at H = 256 and 22 at H = 384
(training model at its initial weights), i.e. only 22–32 levels for h ∈ (−1, 1) in the matrix
products. `check_export.py` shows the cost: H = 384 `int4` (and `tern_o4`, whose output layer
has AO = 22) outputs agree to about 0.013 and the most likely mixture component matches 77% of
the time; H = 256 `int4`: 0.009 and 97%; ternary: 0.002 and 98%. If a 4-bit model is
chosen, training should simulate `hq = floor(h·A)/A` with that `A`.

The QAT model only fake-quantizes weights; the Mac also quantizes activations (hq, Q10/Q14,
table steps of 1/64 or 1/128). `paint/train/eval.py`'s quantized-state simulation measures most of
that gap; the exact Mac numbers come from `ref.py`.

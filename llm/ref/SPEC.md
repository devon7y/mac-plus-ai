# Pixel integer inference: specification (M0)

This is the exact integer arithmetic Pixel runs on the 68000. `model.py` is the normative
implementation (every rounding and truncation below is taken from it); `test_ref.py` checks it.
The Mac app (M5) must produce the same integers; training (M3) should simulate §10.

Run the tests: `python3 llm/ref/test_ref.py` (numpy needed; about 5 s).

## 1. The network (float definition, for training)

| Item | Value |
| --- | --- |
| Layers, width | 6, 160. Pre-norm residual blocks, no biases anywhere |
| Attention | 4 query heads, 2 key/value heads, head dim 40. Query head h reads kv head h // 2 (PyTorch `repeat_interleave(2)` order) |
| Positions | **ALiBi**: score(h, t) = q·k_t / sqrt(40) − slope_h · (pos − pos_t), slopes 1/4, 1/16, 1/64, 1/256. No RoPE, no position embedding |
| Memory | Sliding window of **64 tokens** including the current one (ring buffer; slot = pos mod 64) |
| Norm | RMSNorm with gains: x / sqrt(mean(x²) + 1e-6) · g |
| FFN | 4 experts, router (4×160) picks the argmax (lowest index on ties); output = gate · W_down(silu(W_gate n) ⊙ W_up n), gate = softmax(router logits)[top] (Switch style) |
| Vocabulary | 2,048 tokens; the token embedding is also the output head (tied): logits = E · RMSNorm(x) |
| Parameters | 6,692,640 (ternary 6,359,040; router 3,840; norms 2,080; vocabulary 327,680) |

Float weight layout (what export takes; `nn.Linear` weights are [out, in]):

```
tok_emb [2048,160]   final_norm [160]
layers.{l}.attn_norm [160]   layers.{l}.ffn_norm [160]   layers.{l}.router [4,160]
layers.{l}.wq [160,160]  layers.{l}.wk [80,160]  layers.{l}.wv [80,160]  layers.{l}.wo [160,160]
layers.{l}.experts.{e}.w_gate [512,160]  .w_up [512,160]  .w_down [160,512]
```

Head h of q is rows 40h..40h+39 of `wq`; kv head j is rows 40j..40j+39 of `wk`/`wv`.

## 2. Weight quantization (export.py)

- **Ternary matrices** (wq, wk, wv, wo, every w_gate/w_up/w_down; each separately): BitNet b1.58
  absmean. α = mean|W|, T = clip(round(W/α), −1, 1). Stored: T and α as an SF (§3).
- **Router** (per layer): s = max|R| / 127 as an SF, int8 = clip(round(R / s), ±127).
- **Norm gains** (13 vectors): gs = max|g| / 32767 as an SF, int16 = clip(round(g / gs), ±32767).
- **Embedding / head**: per row v, s_v = max|E_v| / 7; codes = clip(round(E_v / s_v), ±7) (4-bit,
  symmetric; code −8 unused); one exponent eE = ceil(log2(max_v s_v / 32767)) and per-row
  mantissas m_v = clip(round(s_v / 2^eE), 1, 32767). Row v ≈ codes · m_v · 2^eE.
- **ALiBi table**: bias[h][d] = floor(64 · slope_h · d + 0.5), d = 0..63 (int16, stored in the file).

`export.dequantize(pm)` returns float weights holding exactly these values (used for comparisons).

## 3. Scale floats (SF)

Every per-token scale is an SF: a pair (m, e) meaning m · 2^e, with m = 0 (the zero scale, e = 0)
or 2^14 ≤ m < 2^15 (a positive 16-bit word). Operations (sf.py), all truncating:

| Op | Definition |
| --- | --- |
| `norm(v, e)` | v ≥ 0 integer: if bitlen(v) > 15 shift right by bitlen − 15 (truncate), e += that; if < 15 shift left; 0 → zero |
| `from_int(v)` | norm(v, 0) (v < 2^32) |
| `pow2(k)` | (2^14, k − 14) |
| `mul(a, b)` | zero if either is zero, else norm(a.m · b.m, a.e + b.e) (one mulu.w) |
| `div(a, b)` | zero if either is zero (x/0 := 0), else norm(⌊(a.m << 15) / b.m⌋, a.e − b.e − 15); the quotient lies in (2^14, 2^16): one divu.w, never overflows |
| `sqrt_int(v)` | v > 0: shift v by an even amount s into [2^28, 2^30), r = ⌊sqrt⌋ (exact integer square root, in [2^14, 2^15)), result norm(r, s/2) |
| `apply(x, f)` | x a 16-bit integer (−32768..32767): p = x · f.m (muls.w); if f.e < 0, k = −f.e: (p + 2^(k−1)) >> k (arithmetic shift: round half up), or 0 when k ≥ 31; if f.e ≥ 0: p << f.e. Callers clamp |
| compare | zero is smallest; otherwise compare (e, m) lexicographically |

Constants: SQRT_D = (25905, −11) = sqrt(160); ATT_C = (20724, −11) = 64 / sqrt(40).
Export uses `from_float` (nearest SF) only at export time.

## 4. Kernels and overflow bounds

The table kernels (`llm/bench/gen_bench.py`) add 16-bit words with wraparound. Table entries are
built from the inputs by additions and subtractions mod 2^16, so a kernel call returns
**(true dot product) mod 2^16**, and is exact whenever the true result of that call lies in
[−32768, 32767], however large the intermediate table entries or partial sums get. The model keeps
every kernel call within that range:

| Kernel call | Inputs | Worst case | Bound |
| --- | --- | --- | --- |
| Ternary, 160 inputs (QKV, WO, gate/up) | activations ±127 | 160 · 127 | 20,320 |
| Ternary down projection, 512 inputs | ±127, **two calls per row**: groups 0–63 / 64–127 (W4, B4) or 0–50 / 51–102 (W5, B5; 255 + 257 real inputs, padding weights 0) | 257 · 127 | 32,639 per call; the two halves are added in 32 bits |
| Attention score, 40 dims (pair tables) | query codes ±117, key codes ±7 | 40 · 7 · 117 | 32,760 |
| Vocabulary, 160 dims (pair tables) | final codes ±58, embedding codes ±7, **two calls per row** of 80 dims | 80 · 7 · 58 | 32,480 per call; added in 32 bits |
| Weighted sum (wsum), 40 dims × ≤64 positions | per-position weights u_t, value codes ±7 | 7 · Σu_t | Σu_t ≤ 4096 + 32 → 28,896 |
| Router (int8 · int8, 160) | done with muls.w + add.l (32-bit) | 160 · 127 · 127 | 2,580,640 (32-bit) |

So every storage variant (W4, W5, B4, B5) computes identical integers; the variant only changes
speed and memory. `test_ref.py` replays every kernel call of real tokens through emulations of all
four variants and checks the worst cases above.

## 5. One token's forward pass

State: residual r (160 × int32, real value r / 2^16), KV cache per layer, position pos (count of
tokens processed since reset; slot = pos mod 64; valid positions min(pos + 1, 64)).

**Embedding:** r_i = round(code_i · m_v · 2^(eE+16)) (shift left if eE + 16 ≥ 0, else add half and
shift right).

### 5.1 normq(r, g, gs, Q) → (y, β): RMSNorm + absmax quantization

1. M0 = max|r_i| (zero → y = 0, β = zero). sh = bitlen(M0) − 15; t_i = r_i >> sh (or << −sh). |t| ≤ 2^15.
2. ss = Σ ((t_i · t_i) >> 8) (< 160 · 2^22, fits 32 bits). No epsilon (ss ≥ 2^20 whenever M0 > 0).
3. a_i = t_i · g_i (int32). Ma = max|a_i| (zero → zero). s2 = max(0, bitlen(Ma) − 15); b_i = a_i >> s2; Mb = max|b_i|.
4. F = div(from_int(Q), from_int(Mb)); y_i = apply(b_i, F). Then |y| ≤ Q without clamping (F ≤ Q/Mb).
5. β = div(mul(mul(pow2(s2 − 4), gs), SQRT_D), mul(F, sqrt_int(ss))). Then y · β ≈ r / rms(r) · g.

**quant16(x, Q) → (codes, F)** (16-bit x): M = max|x| (zero → zeros, F = zero);
F = div(from_int(Q), from_int(M)); codes = apply(x, F) (|codes| ≤ Q by the same argument).

### 5.2 Attention (layer l)

1. (y1, β1) = normq(r, g_attn, gs_attn, 127). acc = T_qkv · y1 (320 values: q 0–159, k 160–239, v 240–319).
2. sq0 = mul(α_q, β1), sk0 = mul(α_k, β1), sv0 = mul(α_v, β1).
3. For each kv head j: (kc, Fk) = quant16(k_j, 7), (vc, Fv) = quant16(v_j, 7); store codes in the
   current slot with scales sk = div(sk0, Fk), sv = div(sv0, Fv).
4. For each query head h (kv head j = h // 2): (qh, Fq) = quant16(q_h, 117);
   Cq = mul(div(sq0, Fq), ATT_C). For each valid position (distance d = 0..n−1, slot (pos − d) mod 64):
   - S = Σ qh · kc (pair-table kernel, exact).
   - z = clamp(apply(S, mul(Cq, sk_slot)) − bias[h][d], −32768, 32767) (1/64-nat units).
5. Softmax: zmax = max z; p = EXP12[idx(zmax − z)] with idx(δ) = 511 if δ ≥ 1022 else (δ + 1) >> 1
   (round to 1/32 nat); P = Σ p (≤ 64 · 4095).
6. Value weights: over valid slots with nonzero sv, emax = max sv.e (all zero → head output 0, scale
   zero). w_t = (p_t · (sv_t.m >> 3)) >> (emax − sv_t.e) (0 if the shift ≥ 32 or sv_t is zero);
   W = Σ w_t (zero → head output 0). sh = max(0, bitlen(W) − 12); u_t = (w_t + half) >> sh with
   half = 2^(sh−1) (0 when sh = 0). Then Σ u_t ≤ 4096 + 32.
7. wsum: o_d = Σ_t u_t · vc_t,d (per-position tables of 16 words (i − 8) · u_t; exact).
   Head scale so_h = div(pow2(sh + emax + 3), from_int(P)).
8. Requantize the 4 heads into one int8 vector: R_h = mul(from_int(max|o_h|), so_h); Mo = max_h R_h
   (zero → no update). y_o = clamp(apply(o_h, div(mul(so_h, from_int(127)), Mo)), ±127);
   βo = div(Mo, from_int(127)).
9. r = sat32(r + sat32(apply(T_wo · y_o, mul(mul(α_o, βo), pow2(16))))) (sat32 clamps to the int32 range).

### 5.3 Mixture-of-experts feed-forward (layer l)

1. (y2, β2) = normq(r, g_ffn, gs_ffn, 127).
2. Router: rl_e = Σ R_e,i · y2_i (int32). top = first argmax.
3. Gate: sr = mul(mul(s_r, β2), pow2(6)); for each e: δ = apply(1, mul(from_int(rl_top − rl_e), sr))
   (clamped to 65535); Pg = Σ EXP12[idx(δ)]; gate = div(from_int(4095), from_int(Pg)).
4. Expert top: g = T_gate · y2, u = T_up · y2 (exact, ≤ 20,320).
5. gi = clamp(apply(g, mul(mul(α_gate, β2), pow2(6))), −32768, 32767) (gate value in 1/64 units);
   s = SILU64(gi): gi if gi ≥ 512, 0 if gi < −512, else SILU[gi + 512].
6. hh_i = s_i · u_i (int32, < 2^30). Mh = max|hh| (zero → no update); s3 = max(0, bitlen(Mh) − 15);
   (yh, Fh) = quant16(hh >> s3, 127); βh = div(mul(mul(α_up, β2), pow2(s3 − 6)), Fh).
7. dn = T_down · yh (two kernel calls per row, int32, |dn| ≤ 65,024).
8. r = sat32(r + sat32(apply(dn >> 1, mul(mul(mul(α_down, βh), gate), pow2(17))))).

### 5.4 Output (only for the token whose next token is wanted)

1. (yf, βf) = normq(r, g_final, gs_final, 58).
2. lg_v = Σ_d code_v,d · yf_d (two pair-kernel calls of 80 dims; int32).
3. K_v = (lg_v >> 1) · m_v (int32, < 2^30). Real logit_v = K_v · σ, σ = mul(pow2(eE + 1), βf).

Prompt tokens other than the last skip §5.4 (it does not change the state).

## 6. Tables (generated by model.py, embedded in the app)

- **EXP12** (512 × int16): round(4095 · exp(−i/32)); zero from i = 289. No entry is within 1e-6 of a
  rounding tie, so any IEEE platform generates the same table.
- **SILU** (1024 × int16): SILU[j] = round(64 · silu((j − 512)/64)). Beyond the table the identity /
  zero rules are exact (|64 · silu(g) − 64g| < 0.5 for g ≥ 8, |64 · silu(g)| < 0.5 for g ≤ −8).
- **EXPT** (the benchmark's 8-bit table) is verified but no longer used by the model (§12).

## 7. Sampling (fully deterministic given the seed)

- RNG: xorshift32 (x ^= x << 13; x ^= x >> 17; x ^= x << 5, 32-bit), the benchmark's `rand`. Seed 0
  is replaced by 0x2545F491. Exactly one draw per sampled token; greedy draws none.
- Greedy (temperature 0 or top-k ≤ 1): the first v with the largest K_v.
- Otherwise: candidates = the top-k (k ≤ 16) of K sorted by (K descending, token id ascending).
  sT = div(mul(σ, pow2(14)), from_int(T256)) where T256 = temperature · 256 (default 205 = 0.8;
  default k = 16). For each candidate: δ = apply(1, mul(from_int(Kmax − K_c), sT)) clamped to 65535;
  p_c = EXP12[idx(δ)]; tot = Σ p_c (< 2^16). x = next(); pick = ((x >> 16) · tot) >> 16; return the
  first candidate whose running sum of p exceeds pick.
- `model.generate(model, prompt, n, seed, temp256, topk, stop)`: ingest the prompt, then sample.

## 8. Runtime storage (what the 68000 code reads)

All variants compute the same integers (§4); these layouts only change speed and memory.

- **W4 codes** (Plus): int16 per group of 4 weights, **(idx + 81·g)·2 − 32768** with
  idx = Σ_j (w_j + 1)·3^j (j = 0 is the first input of the group), g the group in the row; tables
  are group-contiguous (entry g·81 + idx, built with one store per entry), table base + 32768 in a2.
  (The benchmark used interleaved tables, (idx·G + g)·2 − 32768; same kernel, slower table build.)
- **W5 codes for the expert gate/up matrices** (Plus): (idx + 243·g)·2 − 32768 over groups of 5;
  their 1,024 rows share one 32-group table, so the bigger table pays for itself (20% fewer lookups).
- **B5 codes** (file format, and runtime on the SE): one byte idx = Σ_{j<5} (w_j + 1)·3^j per group of 5;
  the SE splits the down projection into groups 0–50 and 51–102 (rows padded with zero weights).
- **Key cache**: per position and kv head, 40 int16 codes d·32 + (c + 8)·2 − 32768, read through 40
  per-dimension 16-entry tables (c − 8)·q_d built for each query head; plus the SF sk.
  **Value cache**: 40 bytes 2·(code + 8) for `wsum`, plus the SF sv.
- **Vocabulary**: pair codes p·512 + c·2 − 32768 with c = (k_{2p} + 8)·16 + (k_{2p+1} + 8), 80 per row,
  two 40-pair kernel calls per row; the input embedding decodes them.

## 9. Model file (format v2, `export.to_bytes` / `from_bytes`, big-endian)

| Offset | Contents |
| --- | --- |
| 0 | 'PIXL', u16 version 2, u16 header size 64, 9 × i16 dims (160, 6, 4, 2, 40, 512, 4, 2048, 64), i16 eE, 4 × u16 (127, 117, 7, 58), u32 body size, u32 body byte sum, u32 layer record size (0 when entropy coded), u32 offset of layer 0, u16 number of merges, u16 flags (1: Huffman embedding, 2: Huffman ternary bytes), u32 + u32 model checksum (below) |
| 64 | ALiBi bias i16[4][64]; final gain SF (i16 m, i16 e); final gains i16[160]; embedding mantissas i16[2048]; tokenizer merges (3 bytes each: two 12-bit token ids, in rank order; padded to even) |
| | Embedding codes, 2048 × 160 values code + 8: flag 1 set: 16 code lengths, u32 bitstream bytes, canonical Huffman bitstream (MSB first, padded to even); else 2048 × 80 bytes (high nibble = even dim) |
| | Flag 2 set: 243 code lengths + 1 pad byte (one canonical Huffman code for all ternary bytes) |
| layer 0..5 | 19 SFs (gs_attn, gs_ffn, α_q, α_k, α_v, α_o, s_r, then α_gate, α_up, α_down per expert); attention gains i16[160]; FFN gains i16[160]; router i8[4][160]; then 14 ternary matrices (QKV 320×160, WO 160×160, per expert gate 512×160, up 512×160, down 160×512), each a **row-major trit stream packed 5 per byte** (digit = w + 1, first trit lowest), no row padding: flag 2 set: u32 bitstream bytes + Huffman bitstream (+ pad); else the raw bytes |

- **Tokenizer** (llm/train/tokenizer.py): ids 0–3 specials, 4–98 printable ASCII, 99 + r the r-th
  merge. The Mac merges token ids; `export.tokenizer_merges` checks that every token string is
  unique, which makes this identical to the Python string algorithm.
- **Model checksum**: s1 = Σ byte, s2 = Σ s1 (both mod 2^32) over the body of the *uncompressed*
  file (`to_bytes(pm, huff=False)`), so it checks the decoded model, not the coding. The Mac
  computes it while decoding and refuses a damaged model ("The model disks are damaged").
- **Parts**: the Plus needs two floppies. `llm/app/make_disks.py` splits the file into "Pixel Model 1"
  (boot disk, filled until 16 KB stay free) and "Pixel Model 2" (disk 2), each with a 16-byte part
  header ('PXPT', 0, part, u16 2, u32 model id = body byte sum, u32 offset).
- Sizes (random weights): 1,448,780 bytes uncompressed, 1,420,256 entropy coded (the
  embedding codes save about 25 KB, the ternary bytes about 10 KB; trained weights may differ).

## 10. What training (M3) should simulate

Use the float network of §1 with BitNet weight quantization (§2, straight-through estimator) and
fake-quantize activations exactly where the integer path quantizes, each with symmetric
per-vector absmax round-to-nearest (`model.fq`, `FloatModel(qat=True)`):

| Tensor | Range | Grouping |
| --- | --- | --- |
| RMSNorm outputs into QKV / router+experts | ±127 | per token (160) |
| q | ±117 | per token per head (40) |
| k, v (what is cached) | ±7 | per token per kv head (40) |
| attention output (into W_o) | ±127 | per token (160) |
| silu(gate) ⊙ up (into W_down) | ±127 | per token (512) |
| final RMSNorm output (into the head) | ±58 | per token (160) |
| embedding table | ±7 | per row (fixed weights) |

Train with the same sliding window (each query sees keys at distance 0..63, ALiBi), on sequences
longer than 64 tokens so the model learns the steady state. The remaining integer differences (SF
truncation, EXP12 table, SILU table, the gate through EXP12) are small (§11).

## 11. Accuracy and cost (measured with this reference)

- Integer vs the QAT float model (random weights scaled like a trained network, 80 tokens, teacher
  forced, `test_ref.py`): logit correlation **0.9992**, top-1 agreement **100%**; integer vs the
  float model with the same (quantized) weights but float activations: 0.9989 / 100%.
- Random weights at full scale are chaotic: there, perturbing only the embedding by 1% moves the QAT
  float model further (correlation 0.94–0.97) than the integer arithmetic does (0.975–0.984).
- **Measured on the Mac (MAME, 2026-10-16, random weights, bit-exact with this reference):**
  Plus 4 MB (W4): 223 ticks (3.71 s) per generated token at a full 64-token window, 188 ticks
  (3.13 s) per prompt token; kernels about 170 ticks of that (ternary 106, attention 29,
  vocabulary 35, as in the benchmark), the scalar arithmetic of §5 about 50 ticks (0.85 s) even
  with every hot loop in hand-written assembly. Loading takes 137 s (46 s of floppy reads).
- Estimated before the Mac engine existed: per token at a full 64-token window the non-kernel arithmetic is about 30,750 multiply
  operations (norms 6,240; FFN gate/SiLU/requantize 10,176; attention scaling 4,608; router 3,840;
  head row scales 2,048; q/k/v and output requantization 3,840). At the benchmark's measured
  12.3 µs per multiply-and-store element (`misc`), that is about **0.38 s**, against the 0.14 s the
  benchmark budgeted, so the full token estimate is about **3.25–3.3 s** with W4 (the extra down
  projection and vocabulary kernel calls add about 0.03 s). Cheaper options that change no result:
  router as 6 balanced-ternary weight planes through the existing tables (about −0.03 s).

## 12. Decisions and reasons

- **Exact kernels, proven bounds** rather than 32-bit tables: two calls per row where 16 bits are not
  enough (down projection, vocabulary) cost about 0.03 s; 32-bit tables would cost about 0.15 s.
- **Dynamic per-token absmax scales everywhere** (BitNet style) instead of calibrated static scales:
  no calibration data at export, robust to outlier tokens, and simple to simulate in training.
- **ALiBi** for positions: one integer add per score, natural for a sliding window, nothing to
  rotate (RoPE would cost about 0.04 s per token and a sine table).
- **4-bit keys/values with a per-position, per-head scale**: two multiplies per score and one per
  position for the values (about 0.06 s per token); static scales would be cheaper but clip outliers.
- **EXP12 instead of the benchmark's EXPT**: measured attention-output error 0.5–1.1% versus 2–3%,
  same kernel cost (the softmax index uses `(δ + 1) >> 1` and a 512-word table instead of `δ >> 2`
  and 256 words). Also used for the router gate and sampling.
- **Residual stream int32** (Q15.16): no range calibration needed; the norms bring it to 16 bits.
- **Final norm codes ±58** (not ±127) so each 80-dim vocabulary call fits 16 bits; the logit error
  this adds is about 0.05 nat.
- **Top-k ≤ 16** so the sampler's draw is a single 16 × 16-bit multiply.
- No epsilon in the integer RMSNorm (only matters for an all-zero residual, handled explicitly).

## 13. Files

| File | Role |
| --- | --- |
| `sf.py` | scale floats |
| `kernels.py` | bit-exact emulation of the 68000 kernels; W4/W5/B4/B5, pair and value formats |
| `model.py` | integer model (`IntModel`, `sample`, `generate`), float model (`FloatModel`), tables |
| `export.py` | float weights → packed model → model file; random weights; dequantize |
| `verify_dumps.py` | checks the benchmark's MAME dumps with `kernels.py` |
| `test_ref.py` | all tests (one command) |
| `chat.py` | the chat protocol of the Mac app (prompt format, sampling loop, reset) |
| `make_test_tokenizer.py` | stand-in tokenizer (llm/train/tokenizer.py trained on synthetic chats) |
| `../app/` | the Mac app: C engine (`engine.c`), generated 68000 kernels (`gen_kernels.py`), UI (`main.c`) |
| `testdata/` | fresh benchmark dumps from MAME (2026-10-15) and the golden token sequences |

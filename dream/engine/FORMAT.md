# Dream: model file and picture arithmetic (the export contract)

How the Mac makes one picture, exactly. `dream/engine/picture.py` is the bit-exact Python
version (numpy only); `dream/app/engine.c` is the Mac's (generated layer code from
`jit.py` / `bench/jit.c`); `dream/train/student.py` (`mode="int"`) runs the same network.
All integers are big-endian in the file. `>>` is an arithmetic shift (floor).

## Network pass (unchanged from M1; see `ref.py`)

Tensors are int16, channels-last, with a one-pixel zero border. Every layer:

```
acc = wrap16(sum_i w_i * x_i)                      ternary weights (4-bit in the in/out layers of io_q4 models)
v   = wrap16(asr16(acc, k_c) + B[c])               k_c = per-channel shift (asl if negative); two-term gains:
                                                   v = wrap16(wrap16(asr16(acc, k1) + s asr16(acc, k2)) + B[c])
addh: v = wrap16(v + old)
out = clamp(v, lo, hi), v the full int16 value (a 65536-entry table on the 68000)   (raw: out = v)
```

Layer kinds and the layer program are in the file (`LAYR`), so the app needs no rebuild when
the architecture changes. The input `x_t` is the 64x64 (or 32x32) picture in Q9 (value x 512,
clamp +-2047) in 4x4 space-to-depth order: pixel (Y, X) is position (Y/4, X/4), channel
(Y%4)*4 + X%4. The output layer gives x0_hat in Q9, same layout.

## Biases: B[c] for step i and subject s (computed once per step, `ctot` channels)

```
acc   = sum_d E[s][d] * U[d][c]                    32-bit (|acc| <= 32*127*127)
SB[c] = ((acc >> 6) * M[c]) >> 16                  once per picture
B[c]  = wrap16(B0[c] + SB[c] + BT[i][c])           per step
```

Layer l reads `B[off_l .. off_l + cout_l)` in layer order.

## Sampler (N steps, deterministic DDIM on x0_hat, guidance distilled into the network)

```
seed:   xorshift32 state s (never 0): s ^= s << 13; s ^= s >> 17; s ^= s << 5
noise:  for j in 0 .. P/2-1 (P = pixels, in space-to-depth storage order, 2 values per draw):
          r = next(); R = RTAB[r >> 22]  (Q12); a = (r >> 12) & 1023
          n1 = (R * COS[a]) >> 14;  n2 = (R * COS[(a + 768) & 1023]) >> 14        (Q12)
          x[2j] = clamp(n1 >> 3, -2047, 2047);  x[2j+1] = clamp(n2 >> 3, -2047, 2047)   (Q9)
        RTAB[i] = round(4096 * sqrt(-2 ln((i + 0.5) / 1024))), COS[a] = round(16384 * cos(2 pi a / 1024))
step i = 0 .. N-1:
        x0 = clamp(net(x, B_i), -512, 512)                                      (Q9)
        x  = clamp((C1[i] * x + C2[i] * x0 + 8192) >> 14, -2047, 2047)          (Q14 constants)
        re-noising files (flags bit 2): if C3[i] != 0, z = the next P values of the same noise stream
        (xorshift state continues after the initial noise; same Box-Muller pairs) and
        x  = clamp((C1[i] * x + C2[i] * x0 + C3[i] * z + 8192) >> 14, -2047, 2047)
        show x0
C1[i] = round(16384 c sigma_{i+1} / sigma_i), C2[i] = round(16384 (alpha_{i+1} - c alpha_i sigma_{i+1} / sigma_i)),
C3[i] = round(16384 sqrt(1 - c^2) sigma_{i+1})   (c = 1: deterministic DDIM, the default; c = 0: full re-noising)
alpha_i = cos(pi t_i / 2), sigma_i = sin(pi t_i / 2), t_0 = 1 > t_1 > ... > t_N = 0
(the last step has C1 = 0, C2 = 16384: the picture is the last x0).
```

## Display (3x3 halftone, 10 levels)

```
i = clamp(x0 + 512, 0, 1024);  g = clamp(((i - 123) * 10) // 778, 0, 9)     0 = black ... 9 = white
(a contrast stretch: the outer 12% at each end show as solid black / white; display only)
cell pixel (u, v) of a 3x3 cell is white iff ORDER[v][u] < g, ORDER = [[6,1,5],[3,0,8],[7,4,2]]
```

## Model file (`.drm`, type 'DrmM', creator 'Drem')

A sequence of sections: 4-byte tag, u32 byte length, payload (padded to even length).

| Tag | Payload |
| --- | --- |
| `HEAD` | u16 version (2), n_tensors, n_layers, n_tables, n_subjects, emb_dim D, n_steps N, img, patch, flags; u32 ctot. flags bit 0: the network input is x_t >> 3 (Q6, clamp +-255) instead of x_t (Q9); bit 1: two-term gains (SHFT has 2 bytes per channel); bit 2: re-noising sampler (SAMP has C3) |
| `TENS` | n_tensors x (u16 H, W, C); tensor 0 is x_t, tensor 1 is the output |
| `TABS` | n_tables x (i16 lo, hi) |
| `LAYR` | n_layers x 20 bytes: u8 kind (0 pw, 1 dw), mode (0 relu, 1 lin, 2 seth, 3 addh, 4 raw), gather (s2d), scatter (d2s), center, table (255 = raw), wq, pad; u16 src, dst, cin, cout, grid, pad |
| `SHFT` | for each layer, cout x i8 shifts k_c; with flags bit 1, cout x (i8 k1, i8 e2): v = (acc >> k1) + s (acc >> k2) with s = sign(e2), k2 = \|e2\| - 4 (e2 = 0: no second term) |
| `WGTS` | for each layer with weights, starting on a new byte. **4-bit 1x1 convs (LAYR wq = 1):** cout x cin row-major, two per byte, (w0 + 8) << 4 \| (w1 + 8). **Ternary 1x1 convs (pw):** chunk-major 12-bit codes, the order the 68000 compiler consumes them: for each chunk of 7 output rows (o0 = 0, 7, ...), for each input i, code = sum_{j<7} (w[o0+j][i] + 1) * 3^j (rows past cout count as zero weights); two codes per 3 bytes: (a >> 4, (a & 15) << 4 \| b >> 8, b & 255), an odd count padded with 1093 (all zero). **Depthwise 3x3** (unless centre-only): cout x 9 taps row-major, 5 per byte: byte = sum_j (w_j + 1) * 3^j, j = 0..4 (zero-padded) |
| `BIAS` | ctot x i16 B0 |
| `SUBJ` | n_subjects x D x i8 E, then D x ctot x i8 U (d-major), then ctot x i16 M |
| `TIME` | N x ctot x i16 BT |
| `SAMP` | N x (i16 C1, i16 C2, u16 t_i in Q15), or with flags bit 2 (re-noising) N x (i16 C1, i16 C2, u16 t_i, i16 C3) |
| `NAME` | n_subjects Pascal strings (subject names, the picker's list) |
| `END ` | empty |

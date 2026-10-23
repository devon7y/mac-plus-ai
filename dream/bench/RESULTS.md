# Dream kernel benchmark (M1): results (2026-10-16)

One full pass of Dream's denoising network (random ternary weights) timed on MAME's Mac Plus
(ROM v3, 4 MB) with the Mac's 60.15 Hz tick counter, for three 64×64 candidates. Every
configuration was checked **bit-exact** against the Python integer reference
(`dream/engine/ref.py`): the generated code of every layer (size and checksum against
`dream/engine/jit.py`), every layer's output checksum, and the input and output tensors.

Reproduce:

```sh
cmake -S dream/bench -B dream/build/bench -G Ninja -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
cmake --build dream/build/bench
python3 -I toolchain/template/build_disk.py --app dream/build/bench/DreamBA64L.bin --out dream/build/disks/benchA64L.img --no-narrow
mkdir -p dream/build/runs/a64l && BENCH_OUT=$PWD/dream/build/runs/a64l BENCH_SCAN_FROM=40 RAM=4M \
  emu/run_test.sh dream/build/disks/benchA64L.img dream/bench/bench.lua 900 > dream/build/runs/a64l/log.txt
python3 dream/bench/verify.py A64L dream/build/runs/a64l
```

(About 200 s emulated, 12 s wall for A64L.)

## The kernels: ternary weights compiled to 68000 code

Instead of the chatbot's and Doodle's lookup tables, each layer's ternary weights are turned into
68000 code when the model loads (`bench/jit.c`, spec and encodings in `engine/jit.py`). For a 1×1
convolution the code handles one grid position per call: output channels in chunks of 7
(accumulators d1..d7); each input with a non-zero weight in the chunk is loaded once
(`move.w (a0)+,d0`, 8 cycles) and each non-zero weight is one `add.w d0,dj` / `sub.w d0,dj`
(4 cycles); zero weights cost nothing. Then each output is requantised: `asr.w #k` (per-channel
power-of-two gain), `add.w (a2)+` (bias, which carries the subject and the noise level), and a
64 KB clamp table (`add.w dj,dj; move.w 0(a3,dj.w),(a1)+`). Depthwise 3×3: `add.w d(a0),d1` per
non-zero tap. A small driver (`bench/run.s`) calls the code for every grid position. There are no
multiplies and no data-dependent branches, so timing does not depend on the data.

Per weight this costs about 3.8 cycles (2/3 non-zero) against 5.5 cycles plus table building for the W4 lookup
tables, which also have to be rebuilt at every grid position (a conv layer has few outputs per
position). The price is memory: about 1.6 bytes of code per weight, against 0.5 for W4 codes.

## Seconds per network pass (measured)

| Config | Levels (grid: channels / expansion) | MACs/pass | Params | MAME s | Real Plus est. (×1.5) | Code |
| --- | --- | --- | --- | --- | --- | --- |
| A64S | 16: 24/48, 8: 48/96, 4: 96/192 | 5.13M | 147K | **3.56** | 5.3 | 262 KB |
| A64M | 16: 32/64, 8: 64/128, 4: 128/256 | 12.17M | 360K | **7.71** | 11.6 | 627 KB |
| A64L | 16: 32/64, 8: 64/128, 4: 128/256, 2: 256/512 | 16.41M | 1,218K | **9.98** | 15.0 | 2,040 KB |

Two consecutive passes agreed to within 0.2%. "Real Plus" = MAME × 1.5: `SLOW=1.5` takes exactly
a third of every frame's cycles, and this code is CPU/RAM-bound (toolchain/README.md,
"Real-hardware speed").

Throughput by layer type (MAME, A64L): 1×1 convs 1.45M MAC/s at 16×16 (32 channels), 1.72M at
8×8, 1.87M at 4×4, 1.95M at 2×2 (256 channels); depthwise 3×3 0.42–0.50M MAC/s. The cost model in
`engine/arch.py` (instruction timings) predicts every configuration within 1–2%:

```
python3 dream/engine/arch.py              # all configs: MACs, params, estimated seconds, code size
python3 dream/engine/arch.py A64L2        # per-layer table
```

## Choice (approved by the manager, 2026-10-16)

**A64L2** = A64L with the 2×2 level at 192/384 channels: 922K ternary params, 15.2M MACs,
estimated 9.47 s MAME = **14.2 s per pass on a real Plus**; **6 steps ≈ 85 s** per picture (4 steps
57 s, 8 steps 114 s). Code 1.56 MB; whole model in RAM about 2.4 MB (after buffer sharing), so it
runs on the 4 MB Plus and on the SE under System 6, but not inside System 7's 2.27 MB partition.
Fallback: **A64M** (70 s at 6 steps, about 1.1 MB). 1 MB Plus: **A32S** (32×32, 293K params) with
W4 lookup-table kernels for the 1×1 convs (0.5 bytes/weight); estimated 2.7 s per pass with JIT,
roughly 1.7× that with tables, so about 30 s per picture.

## Measured after the pause (2026-10-16 afternoon)

| What | Measured |
| --- | --- |
| A64L2 bench pass, `SLOW=1.5` (real-Plus estimate) | **14.20 s** (model 14.21 s), bit-exact |
| A64M_q4 (4-bit weights, add chains) vs A64M ternary, MAME | 13.04 s vs 7.71 s: **4-bit is 1.69× slower** (model 1.70×), code 1,213 KB vs 627 KB; bit-exact |
| A32S bench pass (JIT), MAME | 1.78 s, bit-exact |

**In the app** (`dream/app`, random models, picture compared bit for bit with `engine/picture.py`):

| | 4 MB Plus, A64L2 (JIT) | 1 MB Plus, A32S (W4 tables) |
| --- | --- | --- |
| Step, MAME / `SLOW=1.5` | 620 / **938 ticks (15.6 s)** | 292 / **442 ticks (7.4 s)** |
| Picture (6 steps + subject setup), `SLOW=1.5` | **about 98 s** | **about 46 s** |
| Load from floppy, `SLOW=1.5` | 57 s (read 26 s, compile about 15 s) | 27.5 s |
| Model RAM; FreeMem after loading | 2.45 MB; 1.49 MB | 448 KB; 342 KB |

The load-time compile went from about 52 s to about 15 s (real Plus) with model files v2: the 1×1
conv weights are stored chunk-major as 12-bit codes (7 ternary weights per code, two codes per 3
bytes), so the compiler copies a precomputed instruction template per code
(`bench/jit.c: jit_pw_codes`; `python3 dream/app/test_jit.py <model>` checks it word for word
against `engine/jit.py` on the host). The 1 MB build first took 10.5 s per step (C requantisation
and depthwise loops); with an assembly requantiser (`app/gen_lut.py: k_rq`) and depthwise layers
compiled with compare-and-branch clamps instead of 64 KB tables (`jit_dw_clamp`, 44 KB of code;
`test_jit.py <model> --clamp`), it takes 7.4 s.

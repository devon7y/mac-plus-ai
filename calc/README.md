# Scribble Calc: a handwriting calculator for a 1986 Macintosh Plus

Write a sum with the mouse, such as `12 × (3.5 + 4) =`. The Mac groups your strokes into symbols,
reads each one with a small ternary neural network, and picks the most likely valid expression.
It then works out the answer exactly in decimal and writes it after the `=` in its own handwriting.
Everything runs on the Plus's 68000 with no FPU, on a 1 MB machine. Reading a symbol takes about
0.24 s on a real Plus (an estimate from the emulator).

Try it at **[macintosh.devon7y.com](https://macintosh.devon7y.com)**: double-click **Scribble
Calc** on the desktop.

## What it does

- **Symbols:** the digits 0–9, `+ − × ÷ = ( )` and the decimal point. Write left to right, one sum
  per line, and end it with `=`.
- **Powers:** write a small, raised number right after a number (`2³ + 1 =`, `(1+2)² =`). Exponents
  are whole numbers. There is no square root.
- **What the Mac read** appears under the paper in Chicago, each symbol under the one you wrote.
- **Fixing a misreading:** click the symbol under the paper. A menu lists all 18 symbols, best
  guesses first, or you can type the right one. **Automatic** removes the fix.
- **Exact arithmetic:** 18 significant digits, 12 shown. `1/3 =` gives 0.333333333333, and
  `7/0 =` gives "Can't divide by zero."
- **Also:** an eraser that removes the strokes you drag over, multi-level Undo, New Line, a tape of
  past results (newest first, click a line to copy its answer) and Edit > Copy.

| Action | How |
| --- | --- |
| Write | Drag on the paper, left to right, one sum per line. End it with **=** |
| Powers | Write the exponent small and raised right after its number (2³) |
| What the Mac read | The line under the paper. The answer follows the = there and is written by hand on the paper |
| Fix a misread symbol | Click it in the line under the paper, then click the right one (best guesses first) or type it. **Automatic** undoes the fix |
| Erase strokes | **Eraser** button (or E), drag over them. **Pen** (or P) to write again |
| Undo | **Undo** button, Delete, or ⌘Z |
| New line | **Clear** button, Return, or **File > New Line** (⌘N) |
| Copy the answer | **Edit > Copy** (⌘C), or click a line on the tape |
| Quit | Esc, or **File > Quit** |

On the website, Ctrl works as the ⌘ key.

## How it reads a line

1. **Grouping strokes into symbols.** Geometric rules decide which strokes belong together: overlap
   in x, where a bar sits, and strokes that touch or cross. That is how `=`, `÷`, `+` and the bar
   of a 5 come together. [`ref/decode.py`](ref/decode.py) is the spec.
2. **Classifying each symbol.** Its strokes are scaled into a 24×24 1-bit raster with a 2×2 brush
   ([`ref/strokes.py`](ref/strokes.py) is the spec), and the network gives a score for each of the
   18 symbols.
3. **Grammar decoding.** A Viterbi search over the symbols' scores picks the most likely *valid*
   expression. Superscripts are a choice inside the same search: the geometry only allows a raised
   symbol to be an exponent, with a bonus of 2 nats. Fixed thresholds gave 5% false exponents.
4. **Your corrections** are fixed points in the search, so the rest of the line is decoded around
   them.

The network runs in idle time, only after 10 ticks with no ink. It checks the mouse button between
rows of a convolution and abandons the pass when you start a new stroke, so no part of a stroke is
lost.

## The model

| | |
| --- | --- |
| Input | one symbol as a 24×24 1-bit raster |
| Layers | 3×3 conv (8 channels) as a 512-entry lookup table, 2×2 max pool, ternary 3×3 conv (24 channels, stride 2), ternary 3×3 conv (48 channels, stride 2), ternary fully connected 32, ternary output 18 |
| Weights | **26,568**: 26,496 ternary (−1, 0, +1), about 53% of them non-zero, plus the first layer's 72, which become the table |
| Arithmetic | 16-bit sums with proven bounds. Each layer ends in `(acc · m + bm) >> 16` (one `muls.w`) and a clamp. Scores are in units of 1/8 nat |
| On the Mac | the weights are compiled into 68000 code when the app is built ([`ref/gen_net.py`](ref/gen_net.py)), one `add.w` or `sub.w` per non-zero weight, so they ship inside the app |
| Model file | [`model/scribble_model.npz`](model/scribble_model.npz), 37 KB |

**Choosing the shape.** Six candidate shapes, from 7,182 to 19,944 weights, were trained in
floating point. They all scored within about a point of each other (97.8% to 98.4% on held-out pen
digits), so capacity barely mattered and the 68000 cost decided. Strided convolutions were about 3×
cheaper than pooling with the same accuracy, and a hidden fully connected layer was added
([`ref/model.py`](ref/model.py) has the specs and a 68000 cost model).

## Data and licences

| Source | Licence | Used for |
| --- | --- | --- |
| [UJI Pen Characters v2](https://archive.ics.uci.edu/dataset/177/uji+pen+characters+version+2) (Prat, Castro, Llorens, Marzal, Vilar), UCI Machine Learning Repository | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) | digits, `( ) . −` and lowercase x as `×`. Pen strokes. 40 writers for training, 20 held out |
| [Pen-Based Recognition of Handwritten Digits](https://archive.ics.uci.edu/dataset/81/pen+based+recognition+of+handwritten+digits), original data (Alpaydin, Alimoglu), UCI Machine Learning Repository | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) | digits. Pen strokes. 30 writers for training, 14 held out |
| Synthetic strokes ([`ref/synth.py`](ref/synth.py)), original | MIT, like the rest of this repository | all 18 symbols, and the only source for `+ ÷ =` |

Both UCI datasets were modified: resampled, slanted and made to look mouse-drawn. The app's About
box credits their authors, UCI and the CC BY 4.0 licence. The raw data is not in this repository.

**Not used:** MNIST and EMNIST (built from NIST Special Databases, which are NIST Standard
Reference Data, "All rights reserved"), HASYv2 (ODbL 1.0, and it is unclear how its share-alike
term applies to trained weights), MathWriting (CC BY-NC-SA) and CROHME (research terms).

**Looking like a mouse.** Every training sample, real or synthetic, goes through the same
degradation before it is rasterized: random slant and size, resampling at mouse rate, and wobble.
Pen data is much smoother than mouse drawing, so this brings the training data closer to what the
app sees. The training set has 221,000 rasters.

## Training

Ten epochs in floating point, then ten epochs of quantization-aware training with ternary weights,
about 6 minutes on a laptop GPU with PyTorch ([`ref/train.py`](ref/train.py)).
[`ref/ref.py`](ref/ref.py) exports the integer model and is the bit-exact reference for it.

## Accuracy

Single symbols from held-out writers, in the Mac's integer arithmetic ("mouse-like" means the same
pen strokes after the mouse degradation):

| Held-out test | Top-1 | Top-3 |
| --- | --- | --- |
| Pen digits, 14 writers, as written (3,498) | 98.31% | 99.77% |
| Pen digits, mouse-like | 97.08% | 99.63% |
| UJI, 20 writers, as written (1,800: digits, `( ) . −` and `×`) | 98.44% | 99.72% |
| UJI, mouse-like | 98.06% | 99.67% |
| Synthetic, an unseen seed, all 18 symbols (5,400) | 99.39% | 99.98% |

**Whole lines** ([`ref/exprset.py`](ref/exprset.py)): random expressions of 15 to 16 symbols on
average, from held-out writers, tightly and randomly spaced, slanted and mouse-like. Two test sets
with different seeds:

| | Seed 7 | Seed 99 |
| --- | --- | --- |
| Lines grouped into symbols exactly right | 91.2% | 85.2% |
| Symbol accuracy on those lines, network alone | 98.71% | 98.35% |
| The same after grammar decoding | **99.02%** | **98.74%** |
| Whole line read exactly right | 79.0% | 76.0% |

About 0.7% of symbols were split or merged by the grouping step.

**Powers** ([`ref/pow_stats.py`](ref/pow_stats.py)): exponents were found correctly in 184 of 200
lines with powers, and 170 of the 200 lines were read entirely right. Ordinary lines got a false
exponent in 2 of 300 (2 of 4,545 symbols).

**In the emulated Mac.** Ten whole lines from held-out writers were drawn through MAME's mouse, one
point per frame. On the Plus, 9 of the 10 lines were read exactly (70 of 71 symbols). The miss, a 4
read as 9, was fixed by clicking it and picking 4. On the SE, 8 of 10 lines were read exactly (69 of
71 symbols). These strokes were held-out writers' pen strokes made mouse-like, not strokes drawn
by a person with a mouse.

**Bit-exact.** The generated classifier, built for the host, matches `ref/ref.py` on all 15,996
test rasters. The Mac's grouping, raster and decoder ([`app/recog.c`](app/recog.c)) with the
classifier match `ref/decode.py` on 300 handwritten lines (3,888 symbols, 94 superscripts) with
random corrections. In MAME, 8 of 8 test rasters matched. The decimal library
([`app/dec.c`](app/dec.c)) matches Python's `decimal` module (18 digits, round half to even) on
215,031 random cases.

## Speed and memory

| | Per symbol (estimate for real hardware) |
| --- | --- |
| Mac Plus, 1 MB | **240 ms** |
| Mac SE, 4 MB | 208 ms |

- On a 1 MB Plus, 678,050 bytes are free after launch. The app is 83.7 KB.
- Redrawing the paper (after an answer changes, an erase or an undo) takes a noticeable fraction of
  a second on a real Plus, since it redraws the ink with QuickDraw lines.

The real-hardware numbers come from MAME slowed by 1.5× (Plus) and 1.3× (SE). MAME leaves out the
video and sound DMA that steals CPU cycles on the real machines, and the factors were calibrated
against it. Scribble Calc has not been run on real hardware yet. The eraser, Undo, the Clear
button, clicking the tape, Edit > Copy and the About box were checked in the host build and by
reading the code, not in the emulator.

## Building

You need [Retro68](https://github.com/autc04/Retro68) built in `~/Retro68-build` (see
[`../toolchain/README.md`](../toolchain/README.md)), `hfsutils`, Python 3 with numpy, and MAME to
run the tests. Training needs PyTorch. Apple's System software is not in this repository. The disk
builder expects `emu/disks/system-6.0/6.0 System Tools.dc42` for the Plus floppy and
`emu/disks/system-6.0.7-tools.dc42` for the SE floppy.

```sh
python3 -I calc/app/make_disks.py
# builds the app in calc/build/app, then
# -> calc/build/release/scribble_calc_plus.img (800K, 1 MB Plus)
#    and scribble_calc_se.img (1.44 MB, SE with System 6.0.7)
```

The build generates the classifier's 68000 code from `model/scribble_model.npz`
(`ref/gen_net.py`) and the answer handwriting from original stroke glyphs (`ref/gen_hand.py`).

To train again, put the UCI files in `calc/data/uji2/ujipenchars2.txt` and
`calc/data/pendigits/pendigits-orig.tra.Z` / `pendigits-orig.tes.Z`, then:

```sh
cd calc/ref
python3 -I build_data.py                                   # rasters -> calc/build/data/
python3 -I train.py --spec S2bM --out ../build/models/S2bM.pt
python3 -I ref.py export ../build/models/S2bM.pt ../model/scribble_model.npz
python3 -I ref.py eval ../model/scribble_model.npz         # accuracy of the integer model
```

Host tests: `python3 -I calc/app/tests/test_dec.py` (decimal arithmetic),
`test_net.py` and `test_recog.py` (bit-exact classifier and decoder, see their headers).

## Files

| Path | What |
| --- | --- |
| [`ref/`](ref/) | Python: stroke data and the exact rasterizer (`strokes.py`), synthetic handwriting and the mouse degradation (`synth.py`), the data builder, model and training, integer export and reference (`ref.py`), grouping and grammar decoding (`decode.py`), the 68000 code generator (`gen_net.py`), the answer handwriting (`gen_hand.py`), line-level tests (`exprset.py`, `seg_stats.py`, `pow_stats.py`) |
| [`model/`](model/) | **the shipped integer model**, `scribble_model.npz` |
| [`app/`](app/) | the Mac app: `main.c` (menus, About box), `draw.c` (paper, strip, tape, correction menu), `line.c` (pen and eraser, undo, scheduling, the answer, the tape), `recog.c` (grouping, raster, decoder), `dec.c` (decimal arithmetic), `make_disks.py` |
| [`app/tests/`](app/tests/) | host bit-exact tests and the MAME test |

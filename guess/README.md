# Guess My Doodle: a 1986 Macintosh Plus guesses what you draw

Guess My Doodle is Google's *Quick, Draw!* game on a Macintosh Plus. The Mac names a thing, you
draw it with the mouse in 20 seconds, and the Mac keeps guessing what it is while you draw. The
guesses come from a small convolutional network with ternary weights, which the Mac compiles into
68000 machine code when the game starts. It knows 150 things, runs on a 1 MB Plus, and takes about
0.9 s per guess on real hardware (an estimate from the emulator).

Try it at **[macintosh.devon7y.com](https://macintosh.devon7y.com)**: double-click **Guess My
Doodle** on the desktop.

## How to play

- The Mac picks a word ("bicycle", "cactus", "The Eiffel Tower") and starts a 20-second clock.
- Draw it in the box. After every stroke, and about every 1.2 s while you are still drawing, the
  panel shows "I see… house?" with the top three guesses and their confidence bars.
- When the top guess is the word, the Mac shows "Oh, I know! It's a house!", a chime plays and the
  round ends early. The clock ticks during the last 5 seconds.
- A game has six rounds. The end screen shows your six drawings, whether the Mac got each one, and
  its three best guesses for each.
- If Apple's MacinTalk file is in the System Folder or next to the game, **File > Speak Guesses**
  says the guesses out loud. MacinTalk is not in this repository.

| Action | Keys |
| --- | --- |
| Draw | Mouse, in the box |
| Start / Go / Next | Return or the big button |
| Undo last stroke | ⌘Z or Delete |
| Clear the drawing | ⌘K |
| Give up a word | Skip |
| Sound on/off | M |
| Speak guesses (with MacinTalk) | **File > Speak Guesses** |
| Back to the title, then quit | Esc |

On the website, Ctrl works as the ⌘ key.

## The model

| | |
| --- | --- |
| Input | the drawing so far, scaled to fit a 32×32 grid of 1-bit pixels |
| Layers | 3×3 conv (16 channels) and 2×2 max pool, then three ternary 3×3 convs with stride 2 (32, 64 and 128 channels), a ternary fully connected layer of 384, and a ternary output layer |
| Classes | 150 of the 345 *Quick, Draw!* categories |
| Weights | about 351,000 ternary weights (−1, 0, +1) with 150 classes. The first layer becomes a table of activations for all 512 possible 3×3 pixel patterns |
| Arithmetic | 16-bit sums of small integer activations (0 to at most 127). Each layer ends in one multiply, add and shift, with batch norm folded in |
| Model file | 100,920 bytes ([`app/data/Guess.gmdl`](app/data/Guess.gmdl)) |
| Generated code | 479,502 bytes of 68000 code, compiled at start-up |

In the spec language of [`train/gnet.py`](train/gnet.py) the shape is
`t16,p,c32k3s2,c64k3s2,c128k3s2,f,l384,o`.

## Data and licence

- **[*Quick, Draw!*](https://github.com/googlecreativelab/quickdraw-dataset)** by Google
  ([quickdraw.withgoogle.com/data](https://quickdraw.withgoogle.com/data)), licensed
  [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). The network was trained on the
  sketch-rnn version of the files (strokes simplified with RDP), preprocessed by
  [`../paint/train/prep_data.py`](../paint/train/prep_data.py) as for Doodle. The drawings were
  rescaled and rasterized for training. The weights are derived from them and are published with
  this attribution, which the game also shows in its About box and in the Read Me on its floppy.
- The dataset and the preprocessed arrays are not in this repository.
- **Which 150 categories.** Thirteen were left out by hand ([`train/exclude.txt`](train/exclude.txt)):
  abstract shapes and textures such as line, squiggle and hexagon, scenes that are hard to draw in
  20 s such as The Great Wall of China, and weapons. The rest were ranked by
  [`train/select_cats.py`](train/select_cats.py). A model trained on all 345 classes was run on
  validation drawings, and the category with the lowest accuracy was dropped again and again. Each
  category was scored by 0.6 × top-1 on whole drawings plus 0.4 × top-1 on half-finished ones,
  with the guess restricted to the categories still in the set. The estimated top-1 on whole
  drawings was 89.6% at 100 categories, 85.0% at 150 and 80.9% at 200. The lists are in
  [`train/cats/`](train/cats/).

## Training

Five candidate shapes, trained in floating point on all 345 categories (30,000 steps of 1,024
drawings, whole drawings, validation split):

| Shape | Multiply-adds | Parameters | Top-1 | Top-3 |
| --- | --- | --- | --- | --- |
| depthwise convs (fpA) | 0.82M | 231K | 64.4% | 81.9% |
| strided 3×3 convs, FC 256 (fpC) | 1.25M | 316K | 66.4% | 83.5% |
| **strided 3×3 convs, FC 384 (fpC2, shipped)** | 1.36M | 426K | 67.5% | 84.3% |
| full 3×3 convs and pools (fpD) | 3.91M | 316K | 68.2% | 84.9% |
| wide depthwise, FC 512 (fpE) | 2.70M | 746K | 69.4% | 85.7% |

The 68000 engine was benchmarked for three of them with random weights. fpA, fpC and fpC2 took
0.63 s, 0.59 s and 0.63 s per guess in unslowed MAME, bit-exact against the Python reference.

The shipped model is fpC2 on the 150 categories:

- **Float:** 120,000 steps. Half the batch is whole drawings and half is random prefixes, cut
  anywhere including mid-stroke, so the network also learns half-finished drawings. Mild aspect
  and rotation augmentation.
- **Quantization-aware:** 60,000 steps with ternary weights (per-channel threshold at 0.7 × mean
  |w|, as in TWN), learned activation step sizes (LSQ) and batch norm frozen for the last 10%.
  [`engine/export.py`](engine/export.py) then folds batch norm into each layer's integer
  requantizer.
- **Tried and not used** (validation, Mac arithmetic, top-1 whole / half): FC 256 at 82.6 / 47.5,
  distillation from the float model at 81.1 / 45.5, a lower learning rate at 81.5 / 46.8. The
  shipped model scored 83.9 / 49.8.
- Trained on the Digital Research Alliance of Canada's GPU clusters.

## Accuracy

On the held-out test split (150 categories × 2,500 drawings), in the Mac's integer arithmetic and
in floating point. "Half" means the first half of the drawing's points.

| Part of the drawing | Mac top-1 | Mac top-3 | Float top-1 | Float top-3 |
| --- | --- | --- | --- | --- |
| whole | **83.95%** | **93.64%** | 87.26% | 95.18% |
| 75% of the points | 75.15% | 90.15% | 79.57% | 92.45% |
| half | **49.68%** | **73.02%** | 55.64% | 77.83% |
| 25% | 17.60% | 36.93% | 21.44% | 42.40% |

## Running it on a 68000

- **Rasterizing.** [`engine/raster.py`](engine/raster.py) is the spec. The bounding box of the
  drawing so far is scaled into the 32×32 grid with a 1-pixel border, and lines are drawn with an
  exact integer DDA. A minimum box side of 48 screen pixels keeps a tiny first stroke from being
  blown up to full size. The Mac ([`app/raster.c`](app/raster.c)), the reference and the GPU
  training rasterizer ([`train/qd.py`](train/qd.py)) follow it exactly.
- **First layer as a table.** A 3×3 conv on a 1-bit image sees only 512 possible patterns. It
  becomes a 512-entry table of 16 activations, done together with the 2×2 max pool in assembly
  ([`app/krun.s`](app/krun.s)).
- **Ternary layers compiled at load time.** [`app/engine.c`](app/engine.c) reads the model and
  writes 68000 code for every layer. Up to 7 output channels are accumulated in `d1`–`d7`. Each
  input is loaded into `d0` once, and each non-zero weight becomes one `add.w d0,dj` or
  `sub.w d0,dj`. Zero weights cost nothing. The file stores 7 ternary weights as one 12-bit code,
  and the compiler expands each code from a table of 2,187 templates.
- **Requantization** is `muls.w #M` / `add.l #B` / `swap` / clamp per output channel.
- **Conv and pool fused.** A conv followed by a 2×2 max pool runs its code on the four positions of
  each window, keeps the largest raw sum per channel and requantizes only that. This is exact
  because the requantizer never decreases.
- **Blank skipping.** In the stride-2 conv right after the table layer, a position whose 8×8-pixel
  window of the drawing is empty gets the empty drawing's output, computed once at load time.
- **No overflow.** The largest 16-bit sum seen was 4,585, about 7× headroom, and the activation
  ceilings make overflow impossible anyway.
- **Drawing while it thinks.** A 60 Hz VBL task samples the mouse, and the network runs in steps of
  about 26 ms between screen updates, so no part of a stroke is lost.

**Bit-exact.** [`engine/ref.py`](engine/ref.py) is the integer reference. In the benchmark, every
tensor of a test drawing and the logits of 16 drawings, dumped from the emulated Mac, match it.
`raster.c` matches `raster.py` on 3,000 drawings, the GPU rasterizer matches it on 3,074, and the
batched GPU integer model used for the accuracy numbers matches `ref.py` on 64 test drawings.

## Speed and memory

- **A guess takes 0.93 s** on a real Plus after a stroke ends (55–56 ticks, measured in the app).
  The network pass alone takes 0.557 s in unslowed MAME.
- **Loading takes about 18 s** on a real Plus: reading the model from the floppy and compiling it.
- **Memory:** 130 KB is still free after loading on a 1 MB Plus.
- It also runs on a Macintosh SE from its 1.44 MB floppy (checked in MAME with 4 MB).

The real-hardware numbers come from MAME slowed by 1.5×. MAME leaves out the video and sound DMA
that steals CPU cycles on a real Plus, and the factor was calibrated against it. The game has not
been timed on a real Plus yet.

## Building

You need [Retro68](https://github.com/autc04/Retro68) (see
[`../toolchain/README.md`](../toolchain/README.md)), `hfsutils`, Python 3 with numpy, and MAME to
run the tests. Training needs PyTorch and a CUDA GPU. Apple's System software is not in this
repository. The disk builder expects `emu/disks/system-6.0/6.0 System Tools.dc42` for the Plus
floppy and `emu/disks/system-6.0.7-tools.dc42` for the SE floppy.

```sh
cmake -S guess/app -B guess/build/app -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
cmake --build guess/build/app        # -> guess/build/app/GuessMyDoodle.bin
python3 -I guess/app/make_disks.py   # -> guess/build/release/guess_plus.img (800K, 1 MB Plus)
                                     #    and guess_se.img (1.44 MB, SE with System 6.0.7)
```

To train a model again from the sketch-rnn files
(`https://storage.googleapis.com/quickdraw_dataset/sketchrnn/<category>.npz`):

```sh
python3 paint/train/prep_data.py <npz folder> <prep folder>
A=t16,p,c32k3s2,c64k3s2,c128k3s2,f,l384,o
C=guess/train/cats/cats_150.txt
python3 guess/train/train.py --prep <prep folder> --arch $A --cats $C --steps 120000 --out runs/c2_fp
python3 guess/train/train.py --prep <prep folder> --arch $A --cats $C --steps 60000 \
  --qat --init runs/c2_fp/ckpt.pt --lr 1e-3 --out runs/c2_q
python3 guess/engine/export.py runs/c2_q/ckpt.pt Guess.gmdl --prep <prep folder> --eval 0
```

## Files

| Path | What |
| --- | --- |
| [`engine/`](engine/) | the specs and host tools: rasterizer (`raster.py`), model file format (`model_file.py`), integer reference (`ref.py`), layer layouts and random test models (`layout.py`), export from a training checkpoint (`export.py`) |
| [`train/`](train/) | data loading and the GPU rasterizer (`qd.py`), network spec and modules (`gnet.py`), `train.py`, category selection (`select_cats.py`, `exclude.txt`, `cats/`), Slurm scripts |
| [`bench/`](bench/) | the 68000 benchmark that compared candidate shapes, checked against `ref.py` |
| [`app/`](app/) | the Mac game: `main.c`, the classifier engine (`engine.c`, `krun.s`), `raster.c`, the mouse sampler (`ink.c`), the sound mixer (`sound.c`), MacinTalk speech (`speech.c`), the disk builder (`make_disks.py`), MAME tests |
| [`app/data/Guess.gmdl`](app/data/Guess.gmdl) | **the shipped model** |
| [`tools/gen_sounds.py`](tools/gen_sounds.py) | the sound effects, synthesized from scratch |

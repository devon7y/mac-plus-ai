# Pixel: a 6.7M-parameter chatbot for a 1986 Macintosh Plus

Pixel is a small language model that runs entirely on a Macintosh Plus: a 7.83 MHz 68000 with no
floating-point unit and 4 MB of RAM. You type a message and press Return. Pixel writes its reply token
by token, at about 0.2 tokens per second on real hardware.

It is a toy on purpose. Pixel is a cheerful little Mac from 1986 that loves floppy disks, beeps
when it's happy, thinks the Chicago font is beautiful, is scared of magnets and isn't quite sure
what year it is. Replies are one or two short sentences, and it forgets anything said more than
64 tokens ago.

![Pixel chatting on the emulated Mac Plus](../docs/images/pixel_screen.png)

Try it in a browser at **[macintosh.devon7y.com](https://macintosh.devon7y.com)**: double-click
**Pixel** on the desktop.

## Sample conversations

These were sampled from the shipped weights (`train/checkpoints/pixel_v2.pxm`) with the reference
implementation, using the Mac's settings: temperature 0.8, top-k 16, 64-token memory. The Mac makes
exactly the same tokens for the same seed (see [Bit-exact](#bit-exact-from-python-to-the-68000)).

| You | Pixel |
| --- | --- |
| what year is it? | I think it is 1986! |
| are you scared of anything | No beep! What scares you? |
| why magnets? | Magnets can be scary for floppy disks. Why are you scared? |
| tell me a joke | Why did the mouse go to the doctor? To get a byte! |
| what is the chicago font | The Chicago font is beautiful! |
| I am sad. My dog is sick. | That's sad! I hope you feel better soon. Beep! |

More: [`train/checkpoints/pixel_v2_samples_all.md`](train/checkpoints/pixel_v2_samples_all.md)
(30 chats, unfiltered).

## The model

| | |
| --- | --- |
| Parameters | **6,692,640** |
| Layers, width | 6 pre-norm transformer blocks, width 160, no biases |
| Attention | 4 query heads, 2 key/value heads (grouped-query), head size 40, ALiBi positions |
| Feed-forward | mixture of experts: 4 SwiGLU experts of width 512 per layer, the router picks one per token |
| Vocabulary | 2,048-token BPE, embedding tied with the output layer |
| Memory | a sliding window of 64 tokens; keys and values cached as 4-bit codes |
| Weights | attention and expert matrices **ternary** (−1, 0, +1; 6.36M of the parameters), embedding 4-bit, router 8-bit |
| File | 1.42 MB (5 ternary digits per byte, partly Huffman-coded), split over two 800K floppies |

The shape came out of a benchmark rather than a guess. Real 68000 kernels for several candidate
shapes and weight packings were timed on an emulated Plus before any training. This design was
the one that fit a 4 MB machine at about 0.33 tokens per second (3 s per token) in the emulator.
MAME later turned out to run a Plus about 1.5× too fast, so on real hardware that's about 0.2
tokens per second. Details:
[`bench/RESULTS.md`](bench/RESULTS.md).

## Training data

Pixel's corpus is synthetic: 1.39 million short chats between a user and Pixel. They were written by
**Qwen2.5-7B-Instruct** (Apache-2.0) running under vLLM on H100 GPUs of the Digital Research
Alliance of Canada ([`data_pipeline/gen.py`](data_pipeline/gen.py)). Each request carried a fixed
persona card plus a random "seed card": topic, the user's writing style, Pixel's mood, the number of
turns and two random words. That kept the chats varied while Pixel stayed consistent.

Small models stay coherent on simple, consistent text, the lesson of TinyStories. So
[`filter.py`](data_pipeline/filter.py) kept only chats whose Pixel turns use a vocabulary of about
2,300 child-level words ([`vocab.txt`](data_pipeline/vocab.txt)), stay short and on-persona, and
aren't duplicates. **706,382 chats (51%) survived.** After truecasing and adding user-side noise
(lower case, typos), the training set is **34.3 million tokens**.

## Training

- **Ternary quantization-aware training** in the BitNet b1.58 style: absmean ternary weights with a
  straight-through estimator, and per-token 8-bit activation quantization that matches the Mac's
  integer arithmetic. Top-1 mixture-of-experts routing with a Switch-style load-balancing loss
  ([`train/model.py`](train/model.py), [`train/train.py`](train/train.py)).
- Batch 256 × 256 tokens, learning rate 2e-3 with cosine decay, the same 64-token window the Mac
  uses.
- **pixel_v2 (shipped):** 40,000 steps, 2.62 billion tokens (76 passes over the corpus), **62
  minutes on one H100**. Best validation loss **2.138** (2.039 on Pixel's turns). A full-precision
  twin of the same shape reached 2.074, so ternary weights cost about 0.07 nats.
- The integer model the Mac runs, scored on 300 validation chats, loses nothing measurable against
  the PyTorch model (2.1342 vs 2.1349). It picks the same top token 96.7% of the time.

## Running it on a 68000

There's no FPU and no fast multiply. A 16 × 16-bit `muls` takes up to 70 cycles, and the whole
machine manages well under a million multiply-adds a second. So the inference engine avoids multiplying
by weights altogether.

- **Ternary matrix × vector through lookup tables.** For each group of 4 (or 5) inputs, the engine
  builds a table of all 81 (or 243) possible signed sums once per token. Each group of weights is
  then a single 16-bit code, and a row costs one `move.w (a1)+,d0` / `add.w 0(a2,d0.w),d1` per group.
  The weights are stored pre-scaled to table offsets.
- **4-bit embedding and 4-bit key/value cache** use 16- and 256-entry tables the same way. Softmax
  uses a 12-bit exponential table.
- **16-bit accumulators everywhere.** Activations are scaled so no sum can overflow, and the
  training-time quantization simulates exactly that.
- Kernels are generated 68000 assembly ([`app/gen_kernels.py`](app/gen_kernels.py) →
  `kernels68k.s`), entered with a computed jump into unrolled blocks. The rest of the engine is
  portable C ([`app/engine.c`](app/engine.c)) built with Retro68.
- The model is loaded from two floppies, Huffman-decoded and unpacked into 16-bit lookup codes,
  which fill most of a 4 MB Plus.

| | Emulator (MAME, unthrottled) | Real hardware (estimate) |
| --- | --- | --- |
| Mac Plus, generating | 0.30 tokens/s | **≈ 0.20 tokens/s** |
| Mac Plus, reading the prompt | 0.36 tokens/s | ≈ 0.24 tokens/s |
| Mac Plus, loading from two floppies | 75 s | ≈ 106 s |
| Mac SE (System 7, smaller packing), generating | 0.24 tokens/s | ≈ 0.18 tokens/s |

The real-hardware numbers come from MAME slowed by 1.5× (Plus) and 1.3× (SE). MAME leaves out the
video and sound DMA that steals CPU cycles on the real machines; the factors were calibrated against
it. On the website, Mini vMac runs at 4× and a reply takes about half a minute.

## Bit-exact from Python to the 68000

[`ref/`](ref/) is the normative definition of the integer arithmetic:
[`ref/SPEC.md`](ref/SPEC.md) and [`ref/model.py`](ref/model.py). It covers the weight packing,
lookup tables, rounding, the exponential table, and sampling with a seeded xorshift32 generator.
Every 68000 kernel was dumped from the emulated Mac and checked against it bit for bit
(`ref/verify_dumps.py`). Whole conversations typed into the real app in MAME produce exactly the
token IDs that [`ref/chat.py`](ref/chat.py) predicts for the same seed. That holds on the Plus and
on the SE, including conversations longer than the 64-token window.

## Files

| Path | What |
| --- | --- |
| [`data_pipeline/`](data_pipeline/) | chat generation (vLLM), filtering, vocabulary, corpus builder; `hpc/` Slurm scripts |
| [`train/`](train/) | tokenizer, model, training, evaluation, `check_ref.py` (PyTorch vs integer model), `int_val.py` |
| [`train/checkpoints/`](train/checkpoints/) | **`pixel_v2.pxm`, the shipped model**, its tokenizer and sample chats |
| [`ref/`](ref/) | the integer inference spec and its Python implementation, tests, chat tool |
| [`bench/`](bench/) | the kernel benchmark that fixed the architecture |
| [`app/`](app/) | the Mac app: engine, kernels, chat UI, disk builders, MAME test scripts |

## Building

You need [Retro68](https://github.com/autc04/Retro68) (see [`../toolchain/README.md`](../toolchain/README.md)),
`hfsutils`, Python 3 with numpy, and MAME to run the tests. Apple's System software isn't in this
repository. The disk builders expect a System 6.0 disk image at
`emu/disks/system-6.0/6.0 System Tools.dc42`; see the top-level README.

```sh
llm/app/build_all.sh llm/train/checkpoints/pixel_v2.pxm
# -> llm/build/app/release/pixel_boot.img + pixel_disk2.img (Mac Plus, two 800K floppies)
#    and pixel_se7.hd (SE hard disk under System 7)
python3 llm/ref/chat.py llm/train/checkpoints/pixel_v2.pxm 4242 205 16 32 "hi pixel"   # the reference
```

`app/make_se_floppy.py` builds the two 1.44 MB floppies for a Macintosh SE. The SE version also has
an on-screen keyboard for machines without one.

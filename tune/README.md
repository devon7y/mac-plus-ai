# Tune: a folk-tune composer for a 1986 Macintosh Plus

Click **Compose** and a small neural network on the Mac writes an original folk tune, note by note.
Each note appears on a staff as it is written, and the Mac plays the tune with a fiddle or flute
voice and a simple bass line. The network is a two-layer GRU with ternary weights, about 400,000
parameters, and it runs on a 1 MB Mac Plus. A typical tune takes 40 to 60 seconds to compose on a
real Plus (an estimate from the emulator). Playback starts by itself as soon as the composer will
stay ahead of it.

Try it at **[macintosh.devon7y.com](https://macintosh.devon7y.com)**: double-click **Tune** on the
desktop.

## What it does

- **Tune types:** Reel, Jig, Hornpipe, Waltz, March, Air, Slip Jig, Strathspey and Polka.
- **Keys:** Any, or 18 keys in major, minor, dorian and mixolydian. **Tempo:** Slow, Moderate,
  Lively or Fast.
- **The staff:** treble clef, key and time signatures, beams by beat, accidentals, rests,
  triplets, repeat signs and first and second endings, four bars to a line. The note being played
  is highlighted.
- **Seeds:** the seed of each tune is shown, and **Tune > Seed…** makes the same tune again.
- **Save** writes the tune as ABC text that TeachText can open. **Copy** puts the staff on the
  clipboard as a picture.

| Action | Keys / mouse |
| --- | --- |
| Compose (the seed shown) / a new tune | Compose button, ⌘K / Again button, Return, ⌘G |
| Play or stop | Play button, Space, ⌘P. Esc stops |
| Type, key, tempo, fiddle/flute, bass | **Tune**, **Key** and **Play** menus |
| Type a seed to make a tune again | **Tune > Seed…** (⌘E) |
| Save as ABC text / copy the staff | Save button, ⌘S / Copy button, ⌘C |
| Sound on/off | M |
| Quit | **File > Quit** |

On the website, Ctrl works as the ⌘ key.

## A tune it wrote

A jig in G (seed 202), sampled with the integer reference, which does the Mac's arithmetic. These
are the notes as ABC:

```
D | GBdefg | dBGGAB | cBcdcB | AFDG2B | GBdefg | dBGG2B | cBcdcB | AFDG2 :| B/c/ | dgdBdB |
cedBAG | c2ccAF | AcBd2B | dgdBdB | deddcB | cBcdcB | AFDG2|]
```

## The model

| | |
| --- | --- |
| Network | 2 GRU layers of 160 units |
| Input | a sum of embedding rows: the previous token (pitch and length), bar position, triplet state, bar and section number, the tune's type, meter and mode, and two look-back inputs (below) |
| Vocabulary | 220 tokens |
| Parameters | **400,060**. 265,600 ternary weights (−1, 0, +1) are used per token |
| Numbers on the Mac | ternary weights packed 5 to a 16-bit code with a 16-bit multiplier per row, hidden state in 8 bits, gate inputs and tables in 16 bits |
| Model file | 378,538 bytes, inside the app as a resource |

**Tokens.** A token is a note (one of 24 scale degrees × 8 lengths), a rest (8 lengths), a pickup
length (7), a bar line (7, including repeats and first and second endings), a sharpen or flatten
mark, a triplet mark, or a start or end mark. Pitches are scale degrees counted from the tonic, so
the model knows nothing about keys and the app transposes to the key you pick.

**Grammar.** A state machine ([`corpus/vocab.py`](corpus/vocab.py), ported line by line to
[`app/grammar.c`](app/grammar.c)) tracks the position in the bar, triplets and the bar and section
counters. It masks out tokens that would break the bar during training and sampling, and it feeds
the model its position inputs.

**Look-back inputs.** The model also sees the note that started at the same bar position four bars
earlier in the section, and the one at the same bar of the previous section. These inputs were the
biggest single improvement, larger than a bigger network (see the table under Training).

## Data and licences

The training data is public-domain melodies from three books published between about 1875 and
1903, in ABC transcriptions from
[trillian.mit.edu/~jc/music/book/](https://trillian.mit.edu/~jc/music/book/). Their terms were
checked before anything was downloaded.

| Source | Transcription | Tunes kept |
| --- | --- | --- |
| O'Neill's *Music of Ireland* (1903) | the O'Neill's Project (1997–2000). Their terms: "You are welcome to download anything here and use it, as long as you give due credit to the transcribers." The files are also under the GNU GPL v2 or later | 1,530 |
| Ryan's *Mammoth Collection* (1883) | the Ryan/Cole team project (R. Davies, J. Chambers and others). No licence is stated | 777 |
| Kerr's *Merry Melodies* 1–4 (Glasgow, c. 1875–1890) | John Chambers (2016–17). No licence is stated | 1,033 |

Thanks to the transcribers of all three collections. The model uses only the notes (pitch, length
and bar lines), which are the public-domain melodies themselves. Decorations, slurs, grace notes
and chord symbols are dropped. The app's About box credits the three books, the O'Neill's Project,
and John Chambers and contributors. The ABC files are not in this repository.
[`corpus/fetch.py`](corpus/fetch.py) downloads them and records their checksums.

**Not used:**

| Source | Why |
| --- | --- |
| The Session (thesession.org) | its licence forbids processing the data with large language model tools, and this project was built with them |
| Nottingham Music Database | no licence is stated on its site, and jukedeck's cleaned copy is GPL-3.0 |
| Essen folksong collection | unclear legal status, with non-commercial permission given only to music21 |
| abcnotation.com, the "irishman" dataset | aggregators of other collections ("irishman" is derived from The Session) |

**Corpus** ([`corpus/abcparse.py`](corpus/abcparse.py), [`corpus/build.py`](corpus/build.py)):
4,836 tunes parsed and **3,340 kept**, 422,017 tokens, a mean of 126 tokens per tune, 180 tunes
held out for validation. Dropped: no tune type (498), duplicates (295), irregular bars (about 400),
key or meter changes (60) and rare note values such as 32nd notes. Kept by type: reel 1,022, jig
732, hornpipe 557, air 496, strathspey 340, march 73, slip jig 71, waltz 41, polka 13. Every tune is
re-centred on its tonic, with the mode chosen by fewest accidentals: major 2,332, minor 418, dorian
333, mixolydian 257. This makes the corpus key-free in place of augmenting it by transposition.

## Training

Training ran on a laptop CPU, with no GPU: 20 epochs in floating point, then 11 epochs of
quantization-aware training with ternary weights (per-row absmean, BitNet b1.58 style) and an
8-bit hidden state ([`train/model.py`](train/model.py), [`train/train.py`](train/train.py)).

| Validation loss (nats per token, grammar-masked) | |
| --- | --- |
| 1 GRU × 192, no look-back (float) | 1.568 |
| 1 GRU × 256, no look-back (float) | 1.532 |
| 2 GRU × 160, no look-back (float, older bar numbering) | 1.505 |
| 2 × 160 with look-back vs without, both at epoch 8 | 1.338 vs 1.553 |
| **2 × 160 with look-back (float)** | **1.2975** |
| the same after quantization-aware training | 1.3153 |
| the integer model, on 60 validation tunes | 1.3718, equal to the quantized float model on the same tunes |

**Memorisation check** ([`ref/memcheck.py`](ref/memcheck.py)). 120 tunes from seeds 1 to 120,
across six of the app's tune types, were compared with every corpus tune. The longest run of
consecutive notes also found in some corpus tune had a median of 9 notes, a 90th percentile of 11
and a maximum of 16 (two bars of a reel shared with "Witch of the Wave"). Corpus tunes compared with
the rest of the corpus share runs with a median of at least 8 and a 90th percentile of at least 16.
The nearest corpus tune by 4-note patterns has a median Jaccard similarity of 0.087 and a maximum of
0.209. No tune was reproduced. The generated tunes averaged 114 tokens and 18 bars.

## Running it on a 68000

[`ref/tuneref.py`](ref/tuneref.py) and [`ref/SPEC.md`](ref/SPEC.md) define the integer arithmetic,
and the Mac computes the same integers.

- **Gate inputs from tables.** The first layer's input is a sum of 11 rows of 480 16-bit values
  looked up from tables. Each entry is clamped so that the sum cannot overflow 16 bits.
- **Ternary matrices through lookup tables**, as in [Pixel](../llm/README.md). For each group of 5
  inputs the engine builds the 243 possible signed sums once per token. Each row then costs one
  table lookup and one add per group. The kernels are generated 68000 assembly
  ([`app/gen_kernels.py`](app/gen_kernels.py)).
- **Gates.** Sigmoid and tanh are 8,192-entry tables. The input sums and gate arithmetic are in
  assembly.
- **Only allowed outputs.** The output layer is computed only for tokens the grammar allows.
- **Sampling** at temperature 1, with tokens more than 4.5 nats below the best (less than 1.1% as
  likely) never picked. A linear congruential generator is seeded with the tune's seed, so the same
  seed and settings always give the same tune.

**Bit-exact.** The host build of the C engine matches the Python reference token for token on 18
tunes ([`ref/test_host.py`](ref/test_host.py)). The 68000 in MAME matches it on 4 tunes and 534
tokens ([`app/bench/check_tokens.py`](app/bench/check_tokens.py)). So does the full app with
sound playing (a reel, seed 4711, 114 tokens).

**Sound.** Tune has its own mixer ([`app/player.c`](app/player.c), [`app/mix68k.s`](app/mix68k.s)).
A VBL task and a Time Manager task fill the Plus's sound buffer with 2 voices at 11.1 kHz. The
melody is a fiddle or flute wavetable (synthesized from harmonics in
[`app/gen_consts.py`](app/gen_consts.py)) with attack, decay, a release gap and vibrato. The bass
follows a small chord rule set. The sequencer runs at interrupt time and keeps tempo by the clock.
It only plays complete bars, so composing can never make it stutter. A WAV recorded from MAME,
slowed to real-Plus speed, held 22.7 s of music with exactly the expected timing and no gap of
150 ms or more.

The clef, accidentals and rests are original 1-bit drawings made by
[`app/gen_glyphs.py`](app/gen_glyphs.py). No font is used.

## Speed and memory

| | Time on real hardware (estimate) |
| --- | --- |
| Mac Plus, one token (benchmark) | 0.35 s |
| Mac Plus, a reel of 114 tokens, in the app with drawing and playback | 46.5 s (0.41 s per token) |
| Mac Plus, a typical tune (100 to 140 tokens) | 40 to 60 s |
| Mac SE, a jig of 111 tokens | 39.9 s |

The mixer costs about 20% of the CPU while it plays. In the benchmark, the recurrent lookups take
68.8 ms per 480 rows, building the tables 19.1 ms and the gates 17.4 ms per layer. On a 1 MB Plus,
235 KB is still free after loading.

These numbers come from MAME slowed by 1.5× (Plus) and 1.3× (SE). MAME leaves out the video and
sound DMA that steals CPU cycles on the real machines, and the factors were calibrated against it.
Tune has not been run on real hardware yet. Not yet checked in the emulator: the Seed… dialog, the
tempo, timbre and bass menu items, and pasting the copied picture into another program.

## Building

You need [Retro68](https://github.com/autc04/Retro68) (see
[`../toolchain/README.md`](../toolchain/README.md)), `hfsutils`, Python 3 with numpy and Pillow,
and MAME to run the tests. Training needs PyTorch. Apple's System software is not in this
repository. The disk builders expect `emu/disks/system-6.0/6.0 System Tools.dc42` for the Plus
floppy and `emu/disks/system-6.0.7-tools.dc42` for the SE floppy.

```sh
tune/app/build_all.sh
# -> tune/build/app/Tune.bin, tune/build/tune_plus.img (800K, 1 MB Plus)
#    and tune/build/tune_se.img (1.44 MB, SE with System 6.0.7)
```

To build the corpus and train again (the shipped shape and schedule):

```sh
python3 -I tune/corpus/fetch.py    # the ABC files -> tune/data/raw/
python3 -I tune/corpus/build.py    # -> tune/data/corpus.json
python3 -I tune/train/prep.py      # -> tune/data/prep.npz
python3 -I tune/train/train.py --H 160 --layers 2 --epochs 31 --qat-from 20 --out tune/build/ckpt/tune.pt
python3 -I tune/ref/export.py tune/build/ckpt/tune.pt tune/build/model    # -> model.npz, model.bin
python3 -I tune/ref/tuneref.py tune/build/model.npz --type reel --key D --seed 1    # prints the tune as ABC
```

The app is built with `tune/model/tune_model.bin` unless CMake is given `-DTUNE_MODEL=<file>`.
[`ref/render.py`](ref/render.py) renders a tune to a WAV with the Mac's synth recipe.

## Files

| Path | What |
| --- | --- |
| [`corpus/`](corpus/) | download (`fetch.py`), ABC parser, corpus builder, the vocabulary and grammar (`vocab.py`) |
| [`train/`](train/) | training arrays (`prep.py`), the PyTorch model, training |
| [`ref/`](ref/) | the integer spec and reference, export to the Mac's format, host and memorisation checks, ABC output, WAV renderer |
| [`model/`](model/) | **the shipped model**: `tune_model.bin` (built into the app) and `tune_model.npz` (for the reference) |
| [`app/`](app/) | the Mac app: `main.c`, the engine (`engine.c`, `gen_kernels.py`), `grammar.c`, the staff (`staff.c`, `gen_glyphs.py`), the player and mixer, ABC saving (`abc.c`), table generator, disk builders, `bench/`, MAME test |

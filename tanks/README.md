# Tank Duel: neural-network tanks on a 1986 Macintosh Plus

Tank Duel is a top-down tank battle in 1-bit graphics. You drive a tank around walled arenas
against up to three enemy tanks. Each enemy is driven by a small neural network, trained by
reinforcement learning, whose weights are all −1, 0 or +1. The Mac runs those networks as
straight-line 68000 code, ten times a second per tank. Shells bounce once off walls, so bank shots
work, for you and for them. It runs on a 1 MB Mac Plus and on a Macintosh SE. On a real Plus it
should hold 30 frames per second with three neural enemies (an estimate from the emulator, slowed
1.5× to match real hardware).

Try it at **[macintosh.devon7y.com](https://macintosh.devon7y.com)**: double-click **Tank Duel** on
the desktop.

## How to play

| Action | Keys |
| --- | --- |
| Drive / turn the hull | W / S, A / D |
| Aim the turret | Mouse (a crosshair) |
| Fire | Click, or Space / Shift |
| Start | Click or Return |
| Pause | Return or P |
| Show the AI's sensors and plans | Tab (or **Options > Show the AI**) |
| Difficulty | **Options**: Easy / Normal / Hard (Normal at first) |
| Sound on/off | M |
| Back to the title, then quit | Esc (File > Quit also works) |

The Plus keyboard reports only two ordinary keys at once. Shift doesn't count, so fire with Shift or
the mouse while you drive and turn.

- **Shells bounce once.** The second wall contact ends a shell. Your own shell can hit you after it
  bounces. Shells that meet destroy each other, and enemy shells only clang off other enemies.
- **Eight arenas, then again.** Stage 1 (Crossing) has one rusher, stage 2 (Corridors) one sniper,
  stage 3 (Pillars) one careful tank. Then Fortress (two rushers), Zigzag (sniper and careful),
  Courtyard (rusher and sniper), Bunkers (two careful tanks and a rusher) and Maze (all three). After
  stage 8 the arenas repeat, with one more enemy where there were fewer than three.
- **Score:** 100 for a rusher, 150 for a sniper, 200 for a careful tank, plus a stage bonus of 500
  that grows by 100 each stage. You start with 3 lives and get one more every 4 stages. A new tank
  starts with a short shield.
- The high score and settings are saved in "Tank Duel Prefs". The title screen plays a silent demo:
  the "player" network drives your tank against a rusher and a sniper.

## The enemies

| Tank | Top speed | Shell speed | Reload | Shells at once | Trained to |
| --- | --- | --- | --- | --- | --- |
| You | 38 px/s | 105 px/s | 0.5 s | 3 | |
| Rusher | 45 px/s | 105 px/s | 0.4 s | 3 | close in fast |
| Sniper | 26 px/s | 150 px/s | 1.0 s | 2 | keep its distance and bank shots |
| Careful | 34 px/s | 113 px/s | 0.67 s | 2 | stay out of your line of fire |

(Rounded from the simulation's units in [`sim/defs.py`](sim/defs.py).)

## How it works

### One simulation, written twice

The game is a deterministic simulation in fixed point. The arena is 384×288 pixels of 16-pixel
tiles. Positions are in 1/16 pixel, angles in 256ths of a turn, and it runs at 30 ticks per second
with up to 4 tanks and 16 shells. Everything is integer: 16-bit state, 16×16 → 32-bit products,
arithmetic right shifts and divisions only of non-negative numbers.

There are two copies of it:

- [`sim/sim.c`](sim/sim.c), the C version. It runs on the Mac (and on the host for tests).
- [`sim/twin.py`](sim/twin.py), a vectorised numpy version that plays thousands of games at once for
  training. It mirrors the C line by line.

All constants, tables, tank types and arenas live in one file, [`sim/defs.py`](sim/defs.py), which
also writes the C tables, so the two can't disagree. They are checked against each other bit for
bit. [`sim/test_bitexact.py`](sim/test_bitexact.py) played 300 random games of 1,800 ticks (545,400
game-ticks) with bots, raw inputs and policy-style actions. The state hash after every tick and every
tank's observation before every tick were identical in C and numpy.

On the emulated Mac, the title screen's demo records the state hash every 30 ticks.
[`tests/demo_ref.py`](tests/demo_ref.py) replays the demo in the numpy twin with the same networks:
all 32 hashes match, and one decision's inputs and 15 output logits from the 68000 code equal the
Python integer model.

### What a tank sees and does

Each enemy decides 10 times a second, and the tanks take turns (tick mod 3) so no two decide on the
same tick. A decision reads 40 numbers in −127..127, from the tank's own point of view:

- 8 sensor rays to the walls, relative to the hull
- its own state (speed, turret angle, cooldown, shells left, shield)
- the nearest enemy in the hull's frame, with relative velocity, distance, aim error and line of
  sight
- a predicted one-bounce shell path: would a shot fired now hit the enemy, or come back and hit me?
- how well the enemy is aiming at me and whether it can fire, the nearest ally, and the 2 nearest
  threatening shells

It answers with 15 logits in four groups: drive (back, stop, forward), turn (left, none, right),
turret (7 steps relative to where it points now, from −24 to +24 of 256) and fire (no, yes).

### The network, compiled to 68000 code

Each policy is a 40 → 32 → 32 → 15 network: 2,784 ternary weights plus an int16 multiplier and an
offset per neuron. Hidden activations are integers from 0 to 127. In the shipped networks 50–60% of
the weights are non-zero (1,385 to 1,670 per network).

A 68000 multiply takes up to 70 cycles, and ternary weights don't need one.
[`app/gen_policy.py`](app/gen_policy.py) compiles each network at build time into straight-line
code. For each neuron:

```asm
        moveq   #0,d0
        add.w   6(a0),d0        | weight +1 on input 3
        sub.w   14(a0),d0       | weight -1 on input 7
                                | weight 0: no instruction at all
        muls.w  #M,d0           | the neuron's multiplier
        add.l   #B,d0           | and offset
        swap    d0              | >> 16, then clamp to 0..127 and store
```

Each weight costs one 12-cycle instruction, and zero weights cost nothing. A whole policy takes
3.5–3.9 ms in MAME. Pixel's lookup tables ([`../llm/`](../llm/)) don't pay off at layers only 32
wide. Compiling at build time gives the effect of Dream's runtime compiler ([`../dream/`](../dream/))
with nothing to do at launch. Each network is about 9 KB of code, and the app carries ten of them.

**Sampling, bit-exact.** Always taking the top action made the sniper, careful and player networks
passive: 25–33% wins against the hunter bot, with half the games timing out. So the Mac samples at
temperature 0.5 with the Gumbel trick in integers. It adds `GUMBEL_Q8[byte] << (SH − 9)` to each
logit and takes the largest in each group. The random bytes come from the simulation's own
xorshift32 generator, which is part of the hashed state, so the Mac and Python still pick exactly the
same actions. With sampling the sniper won 44% against the hunter bot, the careful tank 49% and the
rusher 84%.

## Training

- **PPO** ([`train/ppo.py`](train/ppo.py)) on the numpy twin, with many games per worker process
  ([`train/vec.py`](train/vec.py)). The critic is a float network (40 → 128 → 128 → 1) used only in
  training.
- **Opponents:** three scripted bots (hunter, sitter, wander, which fire less early in training) and
  a league pool of frozen snapshots of all four types, each driving a player-type tank. 40% of the
  games add an ally. Arenas are the game's eight, their mirror images, and randomly generated ones.
- **Rewards** ([`train/env.py`](train/env.py)): +1 for a win, −1 for a loss, −0.5 for a trade,
  plus a style term per type. The rusher is paid for closing in and penalised for standing off. The
  sniper is penalised for coming closer than 130 px and rewarded for bank-shot kills. The careful
  tank is penalised for standing in the enemy's line of fire.
- **Ternary quantization-aware training.** Each layer's weights become −1, 0 or +1 (threshold 0.7 ×
  the mean absolute weight, one scale per neuron), with a straight-through estimator. Activations are
  quantized to 127 steps. The final checkpoint is therefore exactly the integer network the Mac runs
  ([`train/model.py`](train/model.py) has both forms).
- The four types trained at once on one 96-core CPU node of the Digital Research Alliance of
  Canada's clusters, about 80,000 decisions per second per type. No GPU was needed.

The float networks got strong fast: the rusher won 38% against the hunter bot after 10 million
decisions and 84% from 41 million on. Making their weights ternary afterwards dropped that to 23%.
So every shipped network is a quantization-aware fine-tune of a float checkpoint, and the difficulty
level is how far that checkpoint had trained:

| Level | Float checkpoint | Then ternary fine-tuning |
| --- | --- | --- |
| Easy | 10M decisions | 12M |
| Normal | 31M (rusher, sniper), 83M (careful) | 12M–25M |
| Hard | 241M | 200M |

The player network for the title demo was trained like Hard. A longer league run, to 1 billion
decisions per type with its own ternary phase, did not give clearly stronger networks.

No outside data was used. Every training game came from the simulation itself, so the weights are
released with the code under the repository's MIT licence.

### How strong they are

Each shipped network driving its own tank type, sampled as on the Mac, 400 games per cell on the
eight arenas with random spawns and a 60 s limit
([`train/strength.py`](train/strength.py)). Results are win / loss / draw in %, against the scripted
hunter bot and against the player network, both driving a player-type tank.

| Enemy | Level | vs hunter bot | vs player net | Mean distance | Kills by bank shot | Time in line of sight |
| --- | --- | --- | --- | --- | --- | --- |
| Rusher | Easy | 38 / 49 / 14 | 13 / 51 / 36 | 191 px | 62% | 9% |
| Rusher | Normal | 66 / 26 / 8 | 44 / 49 / 8 | 101 px | 26% | 19% |
| Rusher | Hard | 80 / 14 / 6 | 61 / 30 / 9 | 93 px | 20% | 17% |
| Sniper | Easy | 34 / 42 / 25 | 10 / 46 / 43 | 187 px | 65% | 8% |
| Sniper | Normal | 38 / 40 / 23 | 13 / 40 / 47 | 190 px | 52% | 10% |
| Sniper | Hard | 51 / 22 / 27 | 18 / 27 / 55 | 178 px | 50% | 6% |
| Careful | Easy | 23 / 54 / 23 | 8 / 57 / 34 | 194 px | 81% | 7% |
| Careful | Normal | 40 / 22 / 38 | 11 / 32 / 56 | 169 px | 64% | 5% |
| Careful | Hard | 50 / 20 / 29 | 14 / 29 / 57 | 103 px | 53% | 6% |

Easy < Normal < Hard holds against every opponent, though the sniper's Easy and Normal are close.
The styles differ as intended. The Hard rusher fights at 93 px and its games end in about 10 s. The
sniper stays far away and half its kills are bank shots. The careful tank spends 5–7% of the game in
its enemy's line of sight. How hard the levels feel to a human player hasn't been measured.

### Show the AI

Press Tab in a game (or on the title screen) to see what each enemy is thinking, drawn in XOR over
the arena:

- its 8 sensor rays as dotted lines, with a small mark where each ray met a wall
- its predicted shell path, dense when the shot would reach you
- its intended move: a line ahead of or behind the hull, and a dot on the side it is turning to

It shows what the tank sensed at its last decision, so the overlay costs only the plotting. It still
drops a real Plus to about 10 frames per second with three enemies (estimate), while the game itself
keeps running at 30 ticks per second.

## Speed and memory

Measured in MAME slowed to real-Plus speed (1 MB Plus), on stage 8 with three neural enemies:

| | Frames/s | Simulation |
| --- | --- | --- |
| Normal play | 30 (the cap), 22 in the busiest 2 s | 30 ticks/s, none dropped |
| Show the AI on | 10 | 30 ticks/s |

Costs at MAME speed (a real Plus takes about 1.5× as long): an observation 2.4–5.7 ms, a policy
3.5–3.9 ms, a simulation tick 1.1–2 ms, a frame 5–9 ms. Drawing goes to an offscreen buffer, and
only the rectangles that changed are copied to the screen.

- The app is 160 KB, including the ten compiled networks. About 640 KB stays free on a 1 MB Plus.
- **Plus floppy** (800K): a trimmed System 6.0 with [Narrow Screen](../narrowscreen/), 502 KB free.
  It also starts an SE.
- **SE floppy** (1.44 MB): System 6.0.7, 778 KB free. It boots straight into the game and runs at
  30 fps in MAME.
- Sound effects are written straight into the Plus's sound buffer, the method from
  [Rail Runner](../runner/): shots, ricochets, explosions, an engine hum and jingles.

The real-hardware figures are estimates. MAME leaves out the video and sound DMA that steals CPU
cycles on a real Plus, so measurements are slowed 1.5× to match. Tank Duel hasn't been run on a real
Plus or SE yet.

## Building

You need [Retro68](https://github.com/autc04/Retro68) (see
[`../toolchain/README.md`](../toolchain/README.md)), `hfsutils`, a host C compiler, Python 3 with
numpy and PyTorch (the policy compiler imports the model code), and MAME for the emulator tests.
Apple's System software isn't in this repository. See the top-level README for where the disk
scripts expect it.

```sh
python3 tanks/build.py --se        # tanks/build/: app, tankduel.img (800K Plus), tankduel_se.img (1.44 MB SE)
python3 tanks/build.py --random    # the same with untrained networks, to test the pipeline
python3 tanks/sim/test_bitexact.py --games 300 --ticks 1800 --seed 7   # C against numpy, bit for bit
python3 tanks/train/strength.py --games 400 --out strength.md          # the strength table
python3 tanks/tests/watch.py duel.gif --a player=hunter --b rusher=tanks/policies/rusher_hard.npz --ai
                                   # a duel as an animated GIF, with the AI view
SLOW=1.5 emu/run_test.sh tanks/build/tankduel.img tanks/tests/game.lua 90   # frame rate in MAME
```

`tests/game.lua` also takes `STAGE=7` (start at stage 8), `AIVIEW=1` and `BENCH=1`. Training a type
from scratch:

```sh
cd tanks/train
python3 ppo.py --type rusher --out runs/rusher --pool pool --steps 150e6 --workers 22
python3 export.py runs/rusher/final.pt rusher.npz    # the integer network for the Mac
```

`ppo.py` also writes `final_int.npz` itself. Point
[`policies/manifest.json`](policies/manifest.json) at new files and rebuild. The Slurm scripts in
[`train/slurm/`](train/slurm/) ran the four-type league and the fine-tunes on one node.

## Files

| Path | What |
| --- | --- |
| [`sim/`](sim/) | the simulation: `defs.py` (constants, tank types, arenas), `sim.c` (C), `twin.py` (numpy), `gen_tables.py`, `test_bitexact.py` |
| [`train/`](train/) | PPO league training, the ternary model and its integer form, evaluation, strength table, export, Slurm scripts |
| [`policies/`](policies/) | **the ten shipped networks** (7 KB each) and `manifest.json` (type and difficulty to file) |
| [`app/`](app/) | the Mac game: `main.c`, `render.c`, `sound.c`, the policy compiler `gen_policy.py`, sprite generator `gen_art.py`, SE floppy builder |
| [`tests/`](tests/) | the MAME check (`game.lua`), the Mac-against-Python demo replay (`demo_ref.py`), the GIF viewer (`watch.py`) |
| [`build.py`](build.py) | tables, art and policies, Retro68 build, Finder icon, floppies |

All code, arenas, tank designs, sprites and sounds are original. Text uses the Mac's ROM font.

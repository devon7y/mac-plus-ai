# Broadside: sink the Mac's fleet before it sinks yours

Broadside is the classic pencil-and-paper ship game Salvo for a 1986 Macintosh Plus. You hide five
ships on a 10×10 grid, the Mac hides its own, and you take turns firing. The Mac plays at three
levels. On Hard it works out the exact probability that each square holds a ship, over every fleet
that fits what it has seen so far. It does that on the Plus's 7.83 MHz 68000 in under a second per
move (estimated for a real Plus). Broadside runs on a 1 MB Plus under System 6 and on the SE.

Try it at **[macintosh.devon7y.com](https://macintosh.devon7y.com)**: double-click **Broadside**. In
the browser, Control works as ⌘.

## At a glance

| | |
| --- | --- |
| Fleet | Carrier 5, Dreadnought 4, Cruiser 3, Submarine 3, Destroyer 2 |
| Levels | Easy (random hunting), Normal (a density map with noise), Hard (exact probabilities) |
| Rules | standard, or Salvo (one shot per ship afloat), and optionally No Touching |
| Hard's strength | 45.1 shots on average to sink a random fleet (39.2 with No Touching) |
| Hard's speed | 0.66 s per move on average, 0.80 s at most, in a full game on a real Plus (estimate) |
| App | 102 KB (code 36.5 KB, sounds 57 KB), 752 KB free after start on a 1 MB Plus |

## How to play

Place your ships, press Return, then click squares in Enemy waters. A sight closes on the square,
then a splash or an explosion. Hit squares burn. A sunk ship flashes, sounds its horn and is drawn
as a wreck. Sunk ships are revealed to both sides. The Mac comments on the game in a speech bubble
next to a little Mac face.

| Action | Keys |
| --- | --- |
| Move / turn a ship | Drag it / click it, or R with the pointer on it |
| Random fleet, start | Tab, Return (or the buttons) |
| Fire | Click a square in Enemy waters, or W A S D / arrows to aim and Space |
| Salvo rules | Click one square per ship you have afloat. The salvo fires itself. |
| Show the Mac's thinking | T (Options menu) |
| Difficulty | ⌘1 / ⌘2 / ⌘3 (Options: Easy / Normal / Hard), also mid-game |
| Sound on/off | M |
| New game | ⌘N, or Return after a game |
| Statistics | File > Statistics |
| Quit | Esc (the ` key on a Plus keyboard), or File > Quit |

The Options menu also has **Salvo Rules**, **No Touching** (ships may not touch, not even at the
corners, and the water around a sunk ship is marked) and **Speech**. File > Statistics shows games
played, won and lost, your average and best shots per win, and the Mac's per level. The options and
statistics are kept in "Broadside Prefs" in the System Folder.

The board uses 20-pixel squares on the Plus's 512-pixel screen and 18-pixel squares under
[Narrow Screen](../narrowscreen/) (448 pixels).

## How the Mac plays

All of the Mac's play is plain C in [`src/ai.c`](src/ai.c), shared by the Mac app and a host
simulator.

- **Easy** hunts at random. After a hit it tries the neighbours, then the ends of a line of hits.
- **Normal** hunts with the classic density map: for each ship still afloat it counts every
  placement that fits the shots so far, on its own. Noise of up to 8 times the best score is added to the
  choice. It finishes ships like Easy.
- **Hard** uses the exact probability that each square holds a ship, described below.

### Hard: the exact probability map

The Mac knows every hit, every miss and every sunk ship. A fleet is consistent if it fits all of
them (and the No Touching rule when it is on). The chance that a square holds a ship is the share
of consistent fleets that put a ship there. Hard works this out in one of two ways.

- **Exact enumeration** when the count is small: when there are at most 64 possible fleets before
  the last ship is placed, it lists them all. It always tries this with two ships left. Otherwise it
  tries after 8 samples have estimated the count. 44% of moves are exact.
- **Importance sampling** otherwise. Each sample builds a whole fleet. While some hit is not yet
  covered by the sample's ships, the next ship goes through it, chosen from every ship and placement
  that could cover it. The remaining ships follow, largest first. Each sample is weighted by the product of the number of choices
  at each step, so the weighted samples count every consistent fleet equally. The last ship is never
  drawn: all of its placements are added at once (Rao-Blackwellisation). A move uses at least 80
  samples, and more while you think.

While hunting (no hit waiting to be finished), squares off the parity lattice of the smallest ship
left lose 1/16 of their weight. Every ship still afloat covers at least one square of that lattice,
so this gently favours the squares that can find any of them.

[`host/check.c`](host/check.c) checks this against brute force. On random mid-game states with two or
three ships left, it counts every fleet of the remaining ships. Hard's exact counts equal the brute
force (for example 5,533, 5,334, 5,653, 777 and 1,848 fleets) with identical maps, under both rule
sets. The sampler with 20,000 samples is within 0.009 of the exact map everywhere.

### Show the Mac's thinking

Press T and your grid is shaded with the Mac's map in 12 Bayer-dithered greys, from the least to the
most likely square (contrast-stretched). Ships are drawn as haloed outlines, a white sight marks the
Mac's next target, and a line says how it got there, for example "Mac's view: exactly 5,334 possible
fleets" or "312 sampled of about 10^10 fleets".

### Hiding the Mac's fleet

The Mac draws 12 uniformly random fleets and keeps the one with the best score plus a little random
jitter, so its placement stays random. The score avoids squares you tended to shoot early in past
games (a decaying map kept in the prefs file), ships that touch each other, and more than two ships
on the edge.

### Strength

[`host/sim.c`](host/sim.c) plays the same AI code against uniformly random fleets. 3,000 games per
row, shots to sink all five ships, mean ± standard error (median, standard deviation):

| Level | Standard rules | No Touching |
| --- | --- | --- |
| Easy | 64.0 ± 0.3 (63, 16.5) | 54.7 ± 0.2 (55, 11.3) |
| Normal | 49.5 ± 0.2 (49, 11.2) | 44.0 ± 0.1 (44, 7.8) |
| Hard | **45.1** ± 0.2 (45, 9.3) | **39.2** ± 0.1 (39, 6.0) |

In Salvo games, AI against AI (2,000 games per pairing), Hard beats Normal in 77.5% of games as first
player and 58.0% as second, and beats Easy in 85.9% and 72.6%. The first player wins about 60% of
games between equal players. Hard-against-Hard games take 14.5 turns, Normal-against-Normal 17.0.

Some findings from the simulator:

- Strength is flat from 50 to 2,000 samples per move (45.0 to 45.7 shots), so the time went to speed
  instead. A two-step lookahead gained nothing.
- Hunting only on the parity lattice cost 0.3 shots, so parity is only a small tie-break.
- Because sunk ships are revealed, Hard's exact map is no stronger than the noiseless density map in
  single-shot play (44.9 and 39.0 shots). It pays off in Salvo, where several shots go out at once.
  That's why Normal has noise and Easy-style finishing: it sits between Easy and Hard.

## Running it on a 68000

Hard has to sample and enumerate fleets on a machine with no FPU and slow multiply and divide. These
choices keep a move under a second:

- **Bitboards.** Each row and column is a 10-bit mask in a 16-bit word. The free starting squares of a
  ship along a line come from a few shifts and ANDs, and a 1,024-entry table counts the bits.
- **Log-domain fixed point.** Sample weights are products of choice counts. They are kept as base-2
  logarithms in 1/256 units, using lookup tables made by [`tools/gen_tables.py`](tools/gen_tables.py).
  The host and the Mac use the same tables, so they make exactly the same decisions with no floating
  point at all.
- **The last ship in bulk.** Adding all of the last ship's placements at once is the hottest code.
  It has one unrolled loop per ship length. `ai.c` is compiled with `-O2` and the rest of the app with
  `-Os`.
- **No division.** `(row + column) mod L` for the parity lattice is done by subtraction.
- **Thinking in your time.** The Mac samples while you choose your shot. In its own turn it stops
  after 0.75 s once it has 24 complete fleets, because the choice hardly changes after that.

The app's built-in benchmark plays the host simulator's first six games and gets exactly the host's
shot counts (30, 61, 36, 37, 40, 43).

| Hard, measured in MAME and slowed 1.5× to match a real Plus | mean | max |
| --- | --- | --- |
| No time to think ahead (247 moves, 80 samples each) | 0.50 s | 1.21 s |
| A full game with instant replies from you | 0.66 s | 0.80 s |

MAME leaves out the video and sound DMA that steals CPU cycles on a real Plus, so it runs about 1.5×
too fast. The factor was calibrated against it. A profile of Hard (PC sampling) puts 35% of the time
in the last ship's coverage, 19% in the start masks and 13% in the hit options.

## Sound and speech

[`tools/gen_sounds.py`](tools/gen_sounds.py) synthesises the 8 sound effects (fire, splash, explosion,
horn, win, lose, click, ping) from scratch at 11,127 Hz. There are no recorded samples. The app mixes
them itself.

The Mac speaks its comments through MacinTalk when the MacinTalk 1.x file (Apple's speech
synthesizer from 1985, not in this repository) is in the System Folder. It is on none of the release
floppies. Without it the game is silent and the Speech item is dimmed. MacinTalk's programming
interface was never published in Inside Macintosh. [`app/speech.c`](app/speech.c) describes the
driver calls, which were worked out from the MacinTalk 1.31 file and the Talking Moose.

## What has been tested

Verified:

- The posterior against brute force and the strength tables, on the host (above).
- The same results from the same code on the Mac (the benchmark above).
- Full games on autopilot in MAME at real-Plus speed: Hard, Salvo with No Touching, and standard
  rules.
- Real mouse clicks in MAME on the Plus floppy (1 MB, Narrow Screen at 448 pixels) and on the SE
  floppy (SE, 4 MB, System 6.0.7, 512 pixels), plus the About and Statistics windows.
- MacinTalk in MAME, on a test floppy with MacinTalk 1.31 in the System Folder. The driver opens and
  the captured audio has voiced sound at about 128 Hz (the pitch the game sets) right after the
  Mac's first line. Quitting and relaunching opens it again.

Not verified: real hardware, how the sounds and the voice sound to a person, and speech under
System 7.

## Building

You need [Retro68](https://github.com/autc04/Retro68) (see [`../toolchain/README.md`](../toolchain/README.md)),
`hfsutils`, Python 3 and CMake with Ninja, and MAME to run the tests. Apple's System software isn't in
this repository. The disk builder expects the System 6.0 and 6.0.7 disk images listed in the
top-level README.

```sh
cd broadside
cmake -S app -B build/app -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
cmake --build build/app
python3 -I tools/make_disks.py
# -> build/release/broadside.img     800K Mac Plus boot floppy (1 MB Plus, System 6.0 + Narrow Screen)
#    build/release/broadside_se.img  1.44 MB SE boot floppy (System 6.0.7)
#    build/release/Broadside.bin     the application with its Finder icon
```

The host simulator needs only a C compiler:

```sh
cd broadside
cc -O2 -o build/sim host/sim.c src/ai.c src/game.c -lm
build/sim -l 2 -n 3000            # Hard (-l 0 Easy, -l 1 Normal), 3,000 games
build/sim -l 2 -n 3000 -t         # with No Touching
build/sim -salvo -l 2 -l2 1       # Salvo, Hard against Normal
```

`host/check.c` builds the same way and takes `-t` (No Touching) and `-n <states>`. The MAME scripts in
[`tests/`](tests/) say in their headers how to run them, for example
`emu/run_test.sh broadside/build/release/broadside.img broadside/tests/game.lua 900` for a whole game
on autopilot.

## Files

| Path | What |
| --- | --- |
| [`src/game.c`](src/game.c), `game.h` | the rules: fleets, shots, sinking, what each side knows (plain C, shared) |
| [`src/ai.c`](src/ai.c), `ai.h`, `ai_tables.h` | the Mac's targeting (Easy, Normal, Hard) and its fleet placement (plain C, shared) |
| [`host/sim.c`](host/sim.c) | the strength simulator |
| [`host/check.c`](host/check.c) | the brute-force check of Hard's probabilities, exact and sampled |
| [`app/`](app/) | the Mac app (Retro68): `main.c` game flow and menus, `board.c` drawing, `art.c` sprites and the Mac's face, `fx.c` animations, `sound.c` mixer, `speech.c` MacinTalk, `stats.c` prefs and statistics |
| [`tools/gen_sounds.py`](tools/gen_sounds.py) | synthesises the sound effects (run by the CMake build) |
| [`tools/gen_tables.py`](tools/gen_tables.py) | the fixed-point log2, exp2 and bit-count tables |
| [`tools/make_disks.py`](tools/make_disks.py) | the Finder icon and both release floppies |
| [`tests/`](tests/) | MAME scripts: a full game, the AI benchmark, the About and Statistics windows, relaunching |
| [`../toolchain/icons/art/broadside.txt`](../toolchain/icons/art/broadside.txt) | the Finder icon, a warship on waves |

The game, its code, art, sounds and ship names are original. No third-party assets are used.

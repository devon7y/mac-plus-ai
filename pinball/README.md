# Neural Pinball: a pinball table with real physics on a 1986 Macintosh Plus

Neural Pinball is an original pinball table about training an AI, drawn in 1-bit art for a 1 MB
Mac Plus. The ball is simulated, not scripted. Integer physics runs at 120 steps a second, a fast
ball never passes through a wall, and the flippers hit with the real speed of their surface, so
cradles, live catches and passes between the flippers work. When nobody is playing, the Mac plays
the table itself. On a real Plus it should draw 52–59 frames per second with one ball in play (an
estimate from the emulator, slowed 1.5× to match real hardware). It also runs on a Macintosh SE.

Try it at **[macintosh.devon7y.com](https://macintosh.devon7y.com)**: double-click **Neural
Pinball**. Press Return to start a game, or press a flipper key to take over the ball from the Mac.

## How to play

| Action | Keys |
| --- | --- |
| Left / right flipper | A / D (also Z and /, or ← and →) |
| Both flippers | Shift, or the mouse button |
| Plunger | Hold Space or S, let go to launch (a longer pull is stronger) |
| Nudge | W (or Space while the ball is in play). Two warnings, then TILT |
| Start a game | Return. While the Mac plays, a flipper key takes over its ball |
| Pause | P |
| Sound on/off | M (speech: **File > Speech**, when MacinTalk is installed) |
| End the game, then quit | Esc |

Both Shift keys send the same key code on the Plus keyboard, so they can't be separate flippers.
The keyboard reports only two ordinary keys at once, but Shift and the mouse button don't count, so
Shift + W flips both and nudges. The keys are read before every physics step, about every 8 ms, so
flipper timing is finer than the frame rate.

### The rules

- **T-R-A-I-N** rollover lanes. The flippers move the lit letters (lane change). Each full TRAIN
  finishes an **Epoch**: the bonus multiplier goes up, to 5× at most, and the kickback is relit.
  Plunging into the blinking lane is the skill shot.
- Three **neuron** pop bumpers, fed by the lanes.
- **Dropout:** a bank of three drop targets. Knock them all down to light LOCK.
- **Gradient Descent:** a parabolic bowl. Slow balls spiral into its minimum ("CONVERGED"). While
  LOCK, Turing Test or Singularity is lit, its hole catches any ball.
- **Overfit Multiball:** lock two balls in the bowl for three-ball multiball. The **Backprop** ramp
  then scores the **Jackpot** (250,000 × epoch), and after that the bowl holds the **Singularity**
  (1,000,000 × epoch) for 15 s.
- **Attention:** the spinner in the left orbit.
- **Turing Test:** make the ramp, the orbit and the drop bank to light it at the bowl. Then convince
  the robot judge within 40 s with three lit shots, never the same one twice in a row. The little
  robot face on the playfield and the big judge on the side panel react: suspicious, convinced,
  happy, sad. Passing is worth 500,000.
- Ball save (12 s), kickback, tilt, ball search, and an end-of-ball bonus × epoch.
- The top 5 scores, with initials, are saved in "Neural Pinball Scores" next to the app.

The ramp is easiest from the left flipper. The right flipper's shots mostly reach the bumpers, and
the orbit is a hard shot.

## The screen

The table is portrait (256×322 pixels) and the screen is landscape, so the whole table stays on
screen with no scrolling, and a 192-pixel side panel takes the other half: a dot-matrix score
display, the robot judge, messages and the high scores. The flippers are always visible, which
pinball needs. Together the two are 448 pixels wide, the width of
[Narrow Screen](../narrowscreen/), and are centred on a full 512-pixel screen. The ball is 8 pixels
across, true to scale for a table this tall.

The table is drawn from three 1-bit buffers of 32 × 322 bytes. One holds the playfield art. The next
is that art with the lit lamps XORed in (XOR patches can be switched in any order). The last adds the
moving parts: flippers, plunger, spinner and balls. Each frame rebuilds only the rectangles that
changed and copies those to the screen, a few hundred bytes instead of 10 KB.

## The physics

[`core/phys.c`](core/phys.c) is portable C with integer arithmetic only. The same file is compiled
for the host (tests and tools) and for the 68000, so the same inputs give the same game on both.

- **Numbers.** Positions and velocities are 16.16 fixed point, geometry 10.6, normals Q14, and
  contact math Q10.
- **Rate.** 120.3 steps per second, two per Mac tick. It started at 481 Hz and was halved twice for
  speed, with every constant rescaled and every test re-run.
- **Shapes.** Segments with thickness, circles, arcs of up to 180°, sensor lines and discs, one-way
  gates, a ramp layer, and a 16-pixel grid for the broad phase.
- **No tunnelling.** Each step is cut into 1 to 16 sub-moves of at most 3.5 px (|dx| + |dy|, less
  than the 4 px ball radius). Near a moving flipper the ball moves in the flipper's own sub-steps,
  at most 2.5 px of tip travel, but only when the flipper actually sweeps across it. On open table
  one move per step is enough.
- **Contacts.** The ball is pushed out with the overshoot mirrored for bounces. The impulse is
  divided by n·n, which makes elastic bounces exact. Each material (wall, rubber, flipper, sling,
  bumper, drop target, rail) has its own restitution, a resting threshold and Coulomb friction.
  Friction acts only in impacts, so a resting ball rolls instead of sticking.
- **Flippers** are rotating tapered capsules with angular acceleration, a 25 ms coil-rise ramp and
  a release brake, so taps are gentle. At the contact point the ball meets the surface velocity
  ω × r. Contacts along the body need no square root.
- **Kickers and toys.** Slingshots, pop bumpers with ±1.5° of deterministic jitter, drop targets,
  the spinner, the bowl's pull, a sink with a speed limit, the kickback, and a plunger whose strength
  is proportional to the pull.
- **Sleeping balls.** A ball at rest for 0.4 s (cradled, or on the plunger) isn't simulated until
  something can move it. Its position is exact, so nothing visible changes.

### Tested on the host

[`tests/test_phys.c`](tests/test_phys.c) passes all 14 tests:

| Test | Result |
| --- | --- |
| Elastic box, 100 s | worst energy change 1.3% |
| Elastic bounce with gravity | 0.11% per bounce, never a gain |
| Rubber bounces | decay, then the ball rests |
| Thin wall | 0 of 1,062 top-speed shots crossed |
| Closed box | 5 min at the speed cap, stayed inside |
| Swinging flipper | 0 of 648 shots got deeper than 1 px |
| Flipper shots | a flip takes 33 ms, up to 1,874 px/s, aim spans 36–81° by timing |
| Cradle | the ball rests at the inlane corner |
| Live catch | a 232 px/s rebound drops to 0 px/s when the flipper drops at impact |
| Tap pass | a one-tick tap 4.5–10.5 px along the flipper passes to the other cradle |
| Bumper | kicks to 1,359 px/s |
| Plunger | 423 / 1,057 / 1,692 px/s for 4 / 10 / 16 px of pull |
| Bowl | 8 of 8 slow balls sink (0.41 s), 0 of 8 fast ones |
| Determinism | the same hash on two runs |

On the real table, [`tests/sim.c`](tests/sim.c) had the AI play 3,000 balls: none escaped, none got
stuck, and none looped for three minutes. In 300 full AI games
([`tests/gamesim.c`](tests/gamesim.c)) a ball lasted 36 s on average and a game scored 1.37 million.
Per game there were 0.28 multiballs, 0.47 jackpots and 0.79 Turing tests.

**Host and 68000 agree.** A test build plays 3 minutes of a seeded AI game on the Mac before it
starts, and the physics hash (`dab77e55`) is the same on the host and in the emulator.

## The table data

[`tools/table.py`](tools/table.py) holds the table as vector geometry: shapes, ids, materials (speeds
in px/s) and lamp positions, plus a check that walks the inlanes for clearance.
[`tools/art.py`](tools/art.py) draws the 1-bit playfield, lamps, sprites, panel and robot faces from
the same numbers, so the picture and the physics can't drift apart.
[`tools/gen.py`](tools/gen.py) writes both into C. The layout follows the flippers' measured shot
cones ([`tests/aim.c`](tests/aim.c)): the left flipper reaches the ramp, the drop bank and the orbit,
and the right one the bumpers, the lanes and the bowl.

## When the Mac plays

The attract-mode player ([`core/ai.c`](core/ai.c)) is a set of rules, with no learning. It traps
slow balls in a cradle, lets them roll, and flips when the ball reaches a chosen point on the
flipper. Those points come from an aim table that the build generates by running the same physics
([`tests/aimtab.c`](tests/aimtab.c)): cradle a ball, let it roll, flip at each point, and record what
the shot hits. The rules choose the target: the ramp in multiball, the bowl when a lock or the
Singularity is lit, the judge's unconvinced shots during a Turing Test, otherwise the drop bank and
the ramp. When only the other flipper can make the shot, it tap-passes the ball across.

Pressing a flipper key takes over the Mac's ball, with the score and ball count reset. Return starts
a fresh game.

## Sound and speech

Sound effects are square waves and noise written straight into the Plus's 370-sample sound buffer,
the method from [Rail Runner](../runner/). Spoken callouts use MacinTalk 1.x when the MacinTalk file
(Apple's speech synthesizer, not in this repository) is in the System Folder or next to the app. The
glue is Pixel's ([`../llm/app/speech.c`](../llm/app/speech.c)), here in MacinTalk's natural voice
pitched down to 92 Hz. MacinTalk keeps the CPU busy while it talks, so the table pauses for about
1.5–2 s per callout. It speaks only in a player's game and only at big moments: multiball, jackpot,
Singularity, Turing Test start, pass and fail, tilt, ball saved and game over.

## Speed and memory

Measured in MAME slowed to real-Plus speed, on a 1 MB Plus:

| Situation | Frames/s | Physics (ticks/s, 60 = real time) |
| --- | --- | --- |
| Attract mode | 59 | 60 |
| One ball in play | 52–59 | 60 |
| Three-ball multiball | 14–36, depending on the moment | 53–60 |

Multiball falls below 30 fps at times, but the physics stays close to real time. On the SE (MAME, 4
MB, slowed 1.3×) play runs at 60 fps and multiball at 14–20 fps, including a speech pause.

Physics cost per step, in 68000 cycles measured in MAME:

| Scenario | Cycles per step |
| --- | --- |
| Ball on open table | 5,400 |
| Ball in the cradle | 2,300 |
| Fast ball | 10,900 |
| 3 balls, flippers flapping | 18,500 |

Simulating an AI game without drawing takes 26% of real time in MAME, about 39% on a real Plus.

A PC-sampling profiler and MAME instruction traces (in [`tests/`](tests/)) showed where the time
went. The side panel's text drawing first took 57% of it. It now writes whole byte rows, redraws
only changed digits and updates the score at 15 Hz. A physics step costs about 250 instructions for
one ball even in empty space, so the work went into needing fewer of them: fewer steps, longer
sub-moves, a single move on open table, sleeping balls, a full sine table, and flipper sub-steps
only when a flipper sweeps across a ball.

- The app is 100 KB. About 670 KB stays free on a 1 MB Plus.
- **Plus floppy** (800K): a trimmed System 6.0 with Narrow Screen. It also starts an SE.
- **SE floppy** (1.44 MB): System 6.0.7 and the Finder.

The real-hardware figures are estimates. MAME leaves out the video and sound DMA that steals CPU
cycles on a real Plus, so measurements are slowed 1.5× (Plus) or 1.3× (SE) to match. Neural Pinball
hasn't been played on a real Plus or SE yet, so flipper feel, gravity and difficulty haven't been
tuned from real play.

## Building

You need [Retro68](https://github.com/autc04/Retro68) (see
[`../toolchain/README.md`](../toolchain/README.md)), `hfsutils`, a host C compiler (the build runs
the aim-table generator on the host), Python 3 with numpy, and MAME for the emulator tests. Pillow is
needed only for the preview tools. Apple's System software isn't in this repository. See the
top-level README for where the disk scripts expect it.

```sh
python3 pinball/build.py --se      # pinball/build/game/: Neural Pinball.bin, pinball.img (800K Plus),
                                   # pinball_se.img (1.44 MB SE)
python3 pinball/build.py --tag hash --defs HASHTEST   # test build that plays 3 min and stores the physics hash
cd pinball && cc -O2 -Icore tests/test_phys.c core/phys.c -lm -o test_phys && ./test_phys   # physics tests
SLOW=1.5 emu/run_test.sh pinball/build/game/pinball.img pinball/tests/check.lua 140   # frame rate in MAME
```

The disk scripts add MacinTalk to the floppies only when a copy is at
`toolchain/build/demo/extras/macintalk.bin`. Without it the game is silent apart from its sound
effects.

## Files

| Path | What |
| --- | --- |
| [`core/`](core/) | portable C: the physics (`phys.c`), the rules (`game.c`), the attract-mode player (`ai.c`), a sine table |
| [`app/`](app/) | the Mac side: `main.c` (menus, input, timing), `render.c`, `pbsound.c`, `hiscore.c`, `speech.c` |
| [`tools/`](tools/) | the table geometry (`table.py`), art (`art.py`, `font57.py`), C generator (`gen.py`), previews, SE floppy builder |
| [`tests/`](tests/) | host tests (`test_phys.c`, `sim.c`, `gamesim.c`), aim sweeps (`aim.c`, `aimtab.c`), MAME checks, profiling tools |
| [`build.py`](build.py) | table data, aim table, Retro68 build, app name and icon, floppies |

All code, art (including the 5×7 font), sounds and names are original. There is no training data:
the Mac's player is rules plus an aim table measured from the game's own physics.

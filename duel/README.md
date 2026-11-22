# Orbit Duel: a Spacewar!-style duel for one Mac or two

Orbit Duel is a duel between two ships around a star with gravity, in the spirit of *Spacewar!*
(1962). Each ship can turn, thrust, fire torpedoes and jump into hyperspace, which gets riskier with
every jump. The screen wraps around at the edges, and the first to 5 wins. You can play the
computer, a friend on the same keyboard, or link two Macs with a serial cable or over AppleTalk and
play one on each. Both Macs run the same fixed-point simulation in lockstep and exchange only the
players' keys. It runs on a 1 MB Mac Plus, at an estimated 29 frames per second on real hardware
(from the emulator, slowed 1.5×), and on a Macintosh SE under System 6 or System 7.

Try it at **[macintosh.devon7y.com](https://macintosh.devon7y.com)**: double-click **Orbit Duel**,
then press 1 to play the computer or 2 for two players. A game between two Macs needs two real Macs
(or two emulators joined by a serial link, see [Testing](#testing)).

## How to play

On the title screen: **1** play the computer, **2** two players on this keyboard, **3** network game
over AppleTalk, **4** serial cable game (modem ports). **Tab** sets the computer pilot's level
(Easy, Normal or Hard, also in the Options menu). Behind the title, the computer plays itself.

| Action | Keys |
| --- | --- |
| Turn | A / D (or the arrow keys) |
| Thrust | W |
| Fire | Space or Shift |
| Hyperspace (riskier each time) | S |
| Pause (pauses both Macs in a network game) | Return (or P) |
| Sound on/off | M |
| Back to the title, then quit | Esc (on the Plus keyboard, the ` key) |

The Plus keyboard reports only two ordinary keys at once. Shift doesn't count, so turning, thrusting
and firing together works with Shift as the fire key.

**Two players on one keyboard:** the Needle uses W A S D and fires with Space, Option or Command.
The Wedge uses I J K L (or the arrows) and fires with Shift or Enter. The fire keys are modifiers,
which the two-key limit doesn't count.

After a match, Space or Return starts a rematch.

## How it works

### The simulation

[`app/sim.c`](app/sim.c) is the whole game in integer arithmetic. Positions and velocities have 12
fractional bits, the arena is 448×336 pixels and wraps around, and one step is 1/30 s of game time
whatever the frame rate. The random generator lives in the game state, and the code reads nothing
outside it (no clock, no screen). So two Macs given the same seed and the same keys compute the same
game. An FNV hash over every field of the state checks that.

- **Gravity.** The star pulls with a = G / r². A table of G / r³ for each whole-pixel distance turns
  that into one integer square root, one table lookup and a few multiplies per ship per step, with
  no division. Ships start each round in circular orbits 140 px from the star, facing each other.
- **Torpedoes** fly straight, carry the ship's velocity and last 1.2 s. Each ship has up to 4 in
  flight. The star burns up ships within 12 px and swallows torpedoes within 8 px.
- **Hyperspace** takes a ship away for about a second and brings it back at a random spot away from
  the star. The n-th jump in a round explodes with probability n/8, up to 6/8.
- **The computer pilot** has three levels. It keeps away from the star, dodges torpedoes, leads its
  shots and uses hyperspace as a last resort. On the host
  ([`tools/simtest.c`](tools/simtest.c), 200 matches per pairing), Normal against Normal went
  97 : 103 and Hard beat Easy 199 : 1. Rounds last about 10 s and matches 1.5–2 minutes.

### Lockstep between two Macs

[`app/link.c`](app/link.c) keeps two Macs in step by sending only inputs, one byte per player per
step.

- **Input delay.** The keys a Mac reads for step t are used at step t + 3 (100 ms), so they have
  three steps to reach the other Mac. A step runs only when both players' inputs for it are known.
- **Lost packets cost nothing but a resend.** Every packet carries all of this Mac's inputs the
  other hasn't acknowledged yet. A quiet link is kept alive by resending every 8 ticks.
- **Desync check.** Every 30 steps (1 s) each Mac sends a hash of its whole state, and the other
  compares it with its own for the same step. A mismatch shows "OUT OF SYNC". It is detected, not
  repaired.
- **Staying in step.** A Mac that has to wait for the other moves its frame schedule back, so the
  Mac that is ahead falls into step. Both then draw about 30 frames per second.
- **Lost link.** After 3 s of silence the game shows "Waiting for the other Mac...". After 30 s it
  ends.
- **Who hosts.** On a serial cable both Macs send a random number and the higher one hosts. On
  AppleTalk the Mac that invites hosts. The host flies the Needle and picks the seed.

An input packet is 19 bytes plus one per input, about 23 bytes. At 30 packets per second each way
that is about 35% of a 19,200-baud line.

### Serial cable

[`app/serial.c`](app/serial.c) uses the modem port at 19,200 baud, 8 data bits, no parity, no
handshaking, so a three-wire null-modem link is enough. Frames are `0x7E`, length, payload and a
CRC-16. The receiver hunts for `0x7E` and throws away anything with a bad CRC. Writes are
asynchronous with one in flight, and a packet that finds the line busy is dropped (the lockstep layer
resends its contents anyway).

### AppleTalk

[`app/atalk.c`](app/atalk.c) talks to the `.MPP` driver with its own parameter blocks, since
Retro68's interfaces have no AppleTalk header. It works with the Plus ROM's AppleTalk (version 19)
and later versions.

- A **DDP socket** receives the packets. Its listener runs at interrupt time and is written in 68000
  assembly. It reads each datagram into an 8-slot ring. Because of that listener the app is a single
  CODE segment, so it can never be unloaded.
- **NBP** registers each Mac as `<Chooser name> #<node>:Orbit Duel` (or `Mac Plus #12:Orbit Duel`
  when the Mac has no Chooser name) and looks up the others for a "choose an opponent" list. An invitation is
  accepted automatically, since both players have opened the network screen anyway.
- If AppleTalk is off, Orbit Duel says so and offers to turn it on, the same setting as the
  Chooser's "AppleTalk Active" button.

Cables, PhoneNet boxes and the Chooser are explained in [`SETUP.md`](SETUP.md), which is also on the
SE floppy as "Orbit Duel Read Me". In short: a Mac-to-Mac null-modem cable with mini-DIN-8 plugs (an
ImageWriter II printer cable is wired this way) between the modem ports for a serial game, or
LocalTalk or PhoneNet boxes on the printer ports for AppleTalk. `SETUP.md` also describes plugging
the same cable straight between the two printer ports, which is untested.

## Testing

**Serial lockstep, verified in MAME.** [`tests/serial_link.sh`](tests/serial_link.sh) joins two
emulated Pluses through their modem ports (MAME's null modem and a TCP socket), with the computer
flying each Mac's ship. Both reached step 1,800 (60 s), compared checksums 59 times each with no
mismatch and no bad frames, and printed identical hashes. A Plus against an SE booted from the 1.44 MB
floppy also passed (40 and 39 comparisons, no mismatch). MAME's serial clock runs 6.7% fast, so no
standard rate matches its null modem. The tests run at 7,200 baud, where the game sends a packet
every second step and uses a 4-step input delay.

**AppleTalk, verified on one Mac only.** MAME can't link LocalTalk: its serial chip has no SDLC
mode. On one emulated Mac:

- Plus, ROM AppleTalk (version 19): `.MPP` opens, the Mac gets a node number, the DDP socket opens
  with the assembly listener, and the NBP name registers. Lookups run, but version 19 can't send to
  itself, so a Mac never sees its own name.
- SE under System 7.0.1 (AppleTalk version 56): with self-send on, the lookup finds its own name,
  and a datagram sent to its own socket comes back through the assembly listener.

**Not tested:** a game between two Macs over AppleTalk (the invite, accept and start handshake and
play over a real LocalTalk wire), and anything on real hardware: the serial link at 19,200 baud,
LocalTalk, PhoneNet boxes, the direct printer-cable trick, and turning AppleTalk on with a real
PRAM. Mini vMac's LocalTalk-over-UDP build could test two-Mac AppleTalk play in emulators. That
hasn't been tried.

## Speed and memory

- **1 MB Plus, MAME slowed to real-Plus speed**, computer against computer with sound on: 29.0
  frames per second (580 frames in 20 s of Mac time), with the simulation at full speed. 717,190
  bytes stay free after the game starts.
- **Linked games** (two MAME Pluses at normal speed): both Macs drew about 1,785 frames per 1,800
  steps.
- **Drawing.** Three 1-bit buffers (stars, background, frame), each the 448×336 arena plus a 16 px
  margin, and dirty rectangles that split where they wrap around the edges. Each ship has 32
  pre-rotated 16×16 sprites with masks, drawn as 1-pixel lines like a vector display. The arena is the
  same size on both Macs and fits [Narrow Screen](../narrowscreen/)'s 448 pixels.
- **Sound** is written straight into the Plus's sound buffer, the method from
  [Rail Runner](../runner/): torpedoes, explosions, hyperspace sweeps, jingles and an engine hum.
- The app is 56 KB in one CODE segment. Under MultiFinder or System 7 it asks for 384 KB of memory,
  256 KB at least.
- **Plus floppy** (800K): a trimmed System 6.0 with Narrow Screen, 605,696 bytes free. It also
  starts an SE.
- **SE floppy** (1.44 MB): System 6.0.7, the Finder, the Chooser and the read-me, 878,080 bytes
  free. Under System 7, copy the "Orbit Duel" application to the SE's hard disk.

The real-hardware frame rate is an estimate. MAME leaves out the video and sound DMA that steals CPU
cycles on a real Plus, so measurements are slowed 1.5× to match.

## Building

You need [Retro68](https://github.com/autc04/Retro68) (see
[`../toolchain/README.md`](../toolchain/README.md)), `hfsutils`, Python 3, a host C compiler for the
simulation test, and MAME for the emulator tests. Apple's System software isn't in this repository.
See the top-level README for where the disk scripts expect it.

```sh
python3 -I duel/build.py        # duel/build/: Orbit Duel.bin, orbitduel.img (800K Plus),
                                # orbitduel_se.img (1.44 MB SE), orbitduel_se7.hd (System 7.0.1 SE test disk)
python3 -I duel/build.py --no-disks                 # the app only
cd duel && cc -O2 -I app -o build/simtest tools/simtest.c app/sim.c && build/simtest 200
                                # computer against computer on the host, with a determinism check
duel/tests/serial_link.sh 60    # two emulated Pluses over a null-modem link
SLOW=1.5 emu/run_test.sh duel/build/orbitduel.img duel/tests/m0.lua 90   # frame rate at real-Plus speed
```

The System 7 test disk needs `emu/disks/system-7.0.1/se7_base.hd`. Without it the script stops
after the two floppies.

## Files

| Path | What |
| --- | --- |
| [`app/sim.c`](app/sim.c) | the deterministic simulation, the state hash and the computer pilot |
| [`app/link.c`](app/link.c) | the lockstep protocol, the handshakes, the opponent chooser and serial screens |
| [`app/serial.c`](app/serial.c), [`app/atalk.c`](app/atalk.c) | the two transports: modem port, and AppleTalk (`.MPP`, DDP, NBP) |
| [`app/main.c`](app/main.c), [`app/render.c`](app/render.c), [`app/sound.c`](app/sound.c) | menus, title, solo and two-player games, drawing, sound |
| [`tools/`](tools/) | sprite and font generator (`gen_art.py`), Finder icon, SE floppy builder, host simulation test |
| [`tests/`](tests/) | MAME checks: frame rate, keys, the two-Mac serial link, AppleTalk on one Mac |
| [`SETUP.md`](SETUP.md) | cables, Chooser, keys and troubleshooting for a game between two Macs |
| [`build.py`](build.py) | art, Retro68 build, app name and icon, floppies and the System 7 test disk |

All code, art (ships, font, icon) and sounds are original, and no names or art come from
*Spacewar!*.

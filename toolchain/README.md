# Toolchain: Retro68 (C/C++ for the 68000 Mac Plus)

**Status: READY (2026-10-15).** Retro68 is installed and verified on the emulated Mac Plus
(System 6.0, 1 MB and 4 MB, with and without Narrow Screen). New projects should use it
instead of hand-written assembly. Start from `toolchain/template/`.

| What | Where |
| --- | --- |
| Retro68 source | `~/Retro68` (commit `91b5d69`, 2026-10-16; Multiversal submodule `057c802`) |
| Installed toolchain | `~/Retro68-build/toolchain` (`bin/`, `m68k-apple-macos/{include,lib,cmake,RIncludes}`) |
| CMake toolchain file | `~/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake` |
| Mac headers | `~/Retro68-build/toolchain/multiversal/CIncludes` (everything is in `Multiverse.h`; grep it for a trap or type) |
| Compiler | GCC 16.1.0 (`m68k-apple-macos-gcc`, `-g++`), binutils 2.46, newlib 4.6.0, `-mcpu=68000` by default |
| Interfaces | the open-source **Multiversal Interfaces** (Apple's Universal Interfaces are not installed; not needed for System 6 Toolbox work) |
| Template app | `toolchain/template/` (main.c, CMakeLists.txt, build_disk.py, check.lua) |
| Shared library | `toolchain/lib/` (`plus.h`: centred view + fast present, keys, frame pacing; see its README) |
| SE / System 7 testbed | `toolchain/se7/` and the "System 7 / SE testbed" section below |
| Real-hardware Plus disk (BlueSCSI) | `toolchain/plusdisk/` (`build_plus_hd.py`; README for Devon) |
| Tests | `toolchain/tests/` (C++, multi-segment, memory probe), all built by `toolchain/build_all.sh` |
| Build outputs | `toolchain/build/` (gitignored) |

## Environment

CMake builds need **no setup**: the toolchain file points at absolute paths. To run the tools
by hand (`m68k-apple-macos-gcc`, `-objdump`, `-size`, `Rez`, ...):

```sh
export PATH=$HOME/Retro68-build/toolchain/bin:$PATH
```

(That `bin/` also has its own copy of hfsutils 3.2.6, the same version as Homebrew's.)
Other host tools used here: Homebrew `cmake`, `ninja`, `hfsutils`, `mame`, and `python3`.

## Quick start: a new app from the template

```sh
cp -R toolchain/template myproject/app           # anywhere inside this repo
cd myproject/app
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
cmake --build build                    # -> build/HelloPlus.bin (MacBinary application), about 2 s
python3 -I build_disk.py               # -> build/hello_disk.img (800K, boots straight into the app)
../../emu/run_test.sh build/hello_disk.img check.lua 120          # 1 MB: prints RESULT PASS
RAM=4M CHECK_AT=70 ../../emu/run_test.sh build/hello_disk.img check.lua 250   # 4 MB
```

This exact sequence was run on a copy of the template (2026-10-15) and passed. Then rename:
the application name in `CMakeLists.txt` (at most **15 characters**: it goes into the boot
blocks) and its creator code; the magic tag in `main.c` and `check.lua`; and the defaults
of `build_disk.py` (`--app`, `--out`, `--label`).

### What the template app does

`main.c` initialises the Toolbox (`MaxApplZone`, `InitGraf(&qd.thePort)`, `InitFonts`,
`InitWindows`, `InitMenus`, `TEInit`, `InitDialogs`, `InitCursor`), opens a borderless
window (`plainDBox`) the size of `qd.screenBits.bounds`, and draws a 320×240 frame, a
crosshair and "Hello, Plus" centred in `screenBits.bounds` (never assume 512 px: with
Narrow Screen at 32/32 the bounds are `(0,0,342,448)`). It then runs a
`SystemTask`/`GetNextEvent` loop; each click inverts the text box.

It fills a debug block in its globals so a Lua script can check it:

| Offset | Field |
| --- | --- |
| 0 | magic `'HPLS'` (`0x48504C53`) |
| 4 | version (1) |
| 8 | `~magic` (guards against matching the constant inside the code) |
| 12 | state: 0 started, 1 pattern drawn, 1 + n after n clicks |
| 16 | `screenBits.bounds` (top, left, bottom, right; int16) |
| 24 | `screenBits.baseAddr` |
| 28 | `screenBits.rowBytes` |
| 32 | frame drawn (top, left, bottom, right) |
| 40 | `StringWidth`, 42 font ascent |
| 44 | `TickCount()` after drawing, 48 A5, 52 `FreeMem()`, 56 last click (v, h) |

`check.lua` scans RAM for the block (in **2-byte steps**, see gotchas), checks the bounds,
the frame's centring, frame and crosshair pixels in the real frame buffer, the text, that
the Narrow Screen margins are black, then clicks the centre and checks the click arrived.
Screenshots: `$RUN_DIR/shots/hello.png` and `hello_clicked.png`. Environment knobs:
`EXPECT_W` (448 default, 512 for a disk without Narrow Screen), `CHECK_AT` (emulated second
to start looking: 30 at 1 MB, 70 at 4 MB), `CLICK=0`, `SHOT`.

## Building apps with CMake

```cmake
add_application(MyGame main.c render.c data.r   # .r files are compiled by Rez into resources
    TYPE APPL CREATOR MyGm)                     # add CONSOLE for a printf console window
target_compile_options(MyGame PRIVATE -Os -ffunction-sections -Wall -Wno-pointer-sign)
set_target_properties(MyGame PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug")
```

Outputs in the build folder: **`MyGame.bin`** (MacBinary; use this one), `MyGame.APPL`,
`MyGame.dsk` (a small non-bootable HFS disk), `MyGame.ad`, and **`MyGame.code.bin.gdb`**, the
linked ELF with symbols (`m68k-apple-macos-objdump -d`, `m68k-apple-macos-size`).

- `-ffunction-sections` + `-Wl,-gc-sections` drop unused code. `--mac-strip-macsbug` drops
  the MacsBug function names (leave it out while debugging).
- Don't pass `-DCMAKE_BUILD_TYPE=Release` unless you want its `-O3`; the template sets `-Os`.
- Pascal strings: `"\pHello"`. In C they are `char[]`, so passing them to Toolbox calls warns
  under `-Wall` (pointer signedness); the template adds `-Wno-pointer-sign`. C++ is fine.
- Toolbox globals: `qd.screenBits`, `qd.thePort`, ... (`QDGlobals qd` is in the runtime).
- The default `SIZE` resource asks for 1 MB. System 6 without MultiFinder ignores it, but
  System 7 (the SE) and MultiFinder use it: add your own `SIZE -1` in a `.r` file if the app
  needs more (see `~/Retro68/libretro/Retro68APPL.r` for the format).

### Segments (code larger than one CODE resource)

Retro68 builds **multi-segment** applications by default, using the "far model": 32-bit
jump-table entries, and calls and global accesses through absolute addresses relocated at
load time (its runtime patches `_LoadSeg`/`_UnloadSeg`, so this works on System 6 on the
Plus). There is **no 32 KB limit** on a segment, on the jump table or on globals. Verified:

- segments of 70,372 and 281,916 bytes, functions placed more than 32 KB into a segment;
- `UnloadSeg` + heap compaction, then calling again (the segments are reloaded);
- 70 KB of uninitialised globals and 41 KB of initialised data (A5 world).

Default layout: CODE 1 = runtime + libc + libgcc, CODE 2 = your code ("Main"), CODE 3–8 =
libstdc++ pieces and the console library (80-byte empty stubs if unused). To split your
own code, pass a segment map:

```cmake
set_target_properties(MyGame PROPERTIES LINK_FLAGS
    "-Wl,-gc-sections -Wl,--mac-segments -Wl,${CMAKE_CURRENT_SOURCE_DIR}/mygame.segmap")
```

```text
# mygame.segmap: each SEGMENT gets the next CODE id from 3; filters match object files
SEGMENT Battle
    */battle.*
    */battle_ai.*
SEGMENT Overworld
    */overworld.*
```

Everything not matched goes to Main (CODE 2). A segment is loaded on the first call into
it and stays until `UnloadSeg(&someFunctionInIt)`; unloaded segments are purgeable.

For small apps, `-Wl,--mac-single` links one CODE segment (hello world: 4,161 bytes of
resources instead of 7,032; verified running).

### C++

Works (verified at 1 MB and 4 MB): global constructors, virtual calls, `new`/`delete`,
templates, lambdas, `std::array`, `std::sort`, and exceptions (`throw`/`catch` of
`std::runtime_error`). Use `target_compile_features(... cxx_std_17)` (23 is available too).
Size cost, same test program: 51,278 bytes with exceptions, **37,222** with
`-fno-exceptions -fno-rtti`. libstdc++ code goes to CODE 3. `<iostream>` was not tried (it pulls in locale support; expect
it to be large). The libstdc++ build step for the C++20 `std` module failed (the build ignores
it), so `import std;` probably doesn't work; ordinary `#include`s do.

## Making a boot disk

`template/build_disk.py` (copy it with the template; it finds the repo root itself):

```sh
python3 -I build_disk.py --app build/MyGame.bin --out build/mygame.img \
    [--no-narrow] [--level aggressive|safe] [--keep-se] [--label "My Game"] [--extra data.bin ...]
```

`--keep-se` keeps the SE's ROM patch (PTCH 630, +15,728 bytes) in the trimmed System so the
same floppy also boots a Macintosh SE. Without it the SE stops at "This startup disk was
created with a 'Minimum' installer script and will not work on this model Macintosh".
Verified 2026-10-16 on `MACHINE=macsefd RAM=4M`: the hello floppy without the flag never
reaches the app (`check.lua`: TIMEOUT), with it `RESULT PASS` at (0,0,342,512). Narrow
Screen does nothing on the SE. Off by default (Plus-only disks keep the 15 KB).

It extracts the System file from `emu/disks/system-6.0/6.0 System Tools.dc42`, trims it
(`mactools/trim_system.py`, default `aggressive`: 121,986 bytes, ROM fonts only), builds
the disk with `mactools/make_game_disk.py` (System Folder with System + app, boot blocks
naming the app as startup application and shell, no Finder) and copies
`narrowscreen/build/Narrow Screen.bin` into the System Folder unless `--no-narrow`.
`--extra` adds more MacBinary files to the System Folder. Free space left for a 7 KB app:
**670,208 bytes** with Narrow Screen (673,280 without).

Without Narrow Screen, `screenBits.bounds` is `(0,0,342,512)`; with it (32/32 margins in
that file) `(0,0,342,448)`, `ScrnBase` moves right by 4 bytes and the margins are black.

## Running and verifying in MAME

Use `emu/run_test.sh <disk> <script.lua> [seconds]` (read `AGENTS.md`: at most 2 MAME
instances per agent; at 4 MB check after about 65–70 s of emulated time). Pattern for
results: put a block with a 4-byte magic tag, a version and `~magic` in your globals; the
Lua script scans RAM for it (`template/check.lua`, `tests/check_block.lua`), reads fields
by offset, takes `screen:snapshot("name.png")` and prints `RESULT PASS/FAIL`. Look at the
screenshot yourself.

`emu/smoke_all.sh` boots every `emu/games.json` entry that has a disk with `play.sh`'s
settings, two at a time, and writes a labelled contact sheet
(`emu/runs/smoke-<date>/sheet.png`). It prints PASS/FAIL per game: sad Mac, "?" disk, bomb
box, the "won't work on this model" startup alert, a blank or one-colour screen, or a frozen
screen for entries marked `"animates"`. An optional `smoke` field per entry adds shot times
and key/mouse actions (see the script's header).

## System 7 / SE testbed (emulated Macintosh SE FDHD, 4 MB, System 7.0.1)

Matches the lab's SE: MAME machine `macsefd` (ROMs in `emu/roms/macsefd` + `adbmodem`), 4 MB,
a 20 MB SCSI hard disk (ID 6) with a clean **System 7.0.1 Easy Install** made by Apple's own
Installer. The screen is 512×342 (Narrow Screen only acts on the Plus ROM, so it isn't used).

| What | Where |
| --- | --- |
| Install floppies (six 1.44 MB raw images, English) | `emu/disks/system-7.0.1/install/*.img` (archive.org `mac-system-software-7.0.1.7z`) |
| Clean installed hard disk (read-only) | `emu/disks/system-7.0.1/se7_base.hd` |
| Pre-installed 7.0.1 + apps disk, source of the SCSI driver | `emu/disks/system-7.0.1/dl-compilation/hd1.img` (archive.org `mac_MacOS_7.0.1_compilation`) |
| Provenance and checksums | `emu/disks/system-7.0.1/SOURCES.txt` (all gitignored) |
| Scripts | `toolchain/se7/` |

Put an app on a copy of the base disk and run it:

```sh
python3 -I toolchain/se7/build_se_disk.py --app build/MyGame.bin --out build/mygame_se7.hd
MACHINE=macsefd RAM=4M HARDDISK=build/mygame_se7.hd emu/run_test.sh none my_check.lua 300
```

`build_se_disk.py` copies the app into `System Folder:Startup Items`, so the Finder starts
it after booting, as on the real SE (`--shell` makes the app the startup application and
shell instead: no Finder, more memory; `--extra` copies more files to the disk).
`emu/run_test.sh` gained a `HARDDISK=<image>` knob (copied into the run folder like the
floppies; MAME wants a `.hd`/`.hdv`/`.chd` name) and accepts `none` as the floppy.
`template/check.lua` and `tests/check_block.lua` work on both machines:

```sh
B=toolchain/build
MACHINE=macsefd RAM=4M HARDDISK=$B/hello_se7.hd EXPECT_W=512 CHECK_AT=60 emu/run_test.sh none toolchain/template/check.lua 300
MACHINE=macsefd RAM=4M HARDDISK=$B/cpp_se7.hd TAG=CPPT CHECK_AT=60 emu/run_test.sh none toolchain/tests/check_block.lua 300
MACHINE=macsefd RAM=4M HARDDISK=$B/memprobe_se7.hd TAG=MEMP CHECK_AT=80 CLICKS="70:400:146" emu/run_test.sh none toolchain/tests/check_block.lua 300
```

Timing (emulated): the Finder is up at **44 s**; a Startup Items app is drawn by 60 s, so
check at 60 s or later.

**Memory an app gets on the 4 MB SE with System 7.0.1** (`tests/memprobe`, whose SIZE
resource asks for 16 MB, so it receives the largest free block):

| Setup | Largest block for an app |
| --- | --- |
| Finder running (normal use) | **3,101K**: the Finder's own dialog says "MemProbe prefers 16,384K of memory. 3,101K is available"; after OK, `processSize` = 3,174,984 B, `FreeMem` 3,158,414 B inside it |
| App as startup shell, no Finder (`--shell`) | 3,489,196 B (3,407K), `FreeMem` 3,472,678 B |
| Default Retro68 SIZE (1 MB) | the app gets 1,024K (`FreeMem` 1,031,522 B in the hello app) |

The real SE showed **2,267K**, about 830K less than this clean install. Likely reasons:
more extensions or control panels loaded at startup, File Sharing or AppleTalk turned on,
a larger disk cache (Memory control panel), or fonts and sounds added to the System file.
Checking "About This Macintosh" and the Extensions folder on the real SE would settle it.
Plan for **about 2.2 MB** on the real machine. Set your own SIZE resource (as in
`tests/memprobe/memprobe.r`) when an app needs more than 1 MB: System 7 always uses it;
if the preferred size isn't free it shows the dialog above.

How the base disk was made (reproducible, about 3 min wall time, 750 s emulated):

```sh
toolchain/se7/install_system.sh emu/disks/system-7.0.1/se7_base.hd
```

1. `make_blank_hd.py`: copy the driver descriptor map, partition map and Apple SCSI driver
   (blocks 0–95) from the compilation disk, zero the rest, `hformat` the HFS partition.
2. `install701.lua`: boot "Install 1" on macsefd with the blank disk attached, click OK,
   Switch Disk (to Macintosh HD), Install; whenever the drive has been empty for 1.5 s,
   insert the next floppy in a cycle (the Installer ejects a wrong one); recognise the
   "Installation ... was successful" alert from its pixels, click Quit (a clean shut down).
   Easy Install installed System Software, all printing software and File Sharing
   (System file 1,876,401 bytes).
3. `firstboot.lua`: boot from the hard disk (the Finder builds its desktop file and the
   Startup Items folder), then Special > Shut Down.

`toolchain/se7/se_lib.lua` (mouse, clicks, floppy swaps, RAM size for both machines) and
`toolchain/se7/steps.lua` (timed `click`/`snap`/`insert` lines from a text file) are there
for scripting other SE sessions.

## Real-hardware speed (MAME is about 1.5x too fast for the Plus)

**MAME 0.289 runs the Plus's and SE's 68000 at the full 7.83 MHz.** A real Plus loses about a
third of its RAM bandwidth to the video and sound fetches. Use **`SLOW=1.5`** (Plus) or
**`SLOW=1.3`** (SE) with `emu/run_test.sh` to see real-machine frame rates.

Findings (2026-10-16):

1. **MAME's contention code has no effect.** MAME does have it:
   `src/mame/apple/mac128.cpp` (tag mame0289):
   - `mac_scanline` (lines 474-499) calls `m_maincpu->adjust_icount(-128)` on every visible line,
     with a comment describing the CPU/video 4+4-cycle interleave;
   - `mac_hblank` (line 515) calls `adjust_icount(-4)` for the sound fetch.

   But both are timer callbacks, and `src/emu/diexec.h:159` is
   `void adjust_icount(int delta) noexcept { if (executing()) *m_icountptr += delta; }`, so
   outside the CPU's timeslice they do nothing. The SE (`macse`, line 1306) inherits the same
   code.

   Measured with `tests/cyclecal` (loops of known cycle counts, timed with Ticks) on both
   `macplus` and `macsefd`: **7.79 MHz effective** for register code and for RAM copies alike
   (99.4% of 7.8336 MHz; the rest is interrupts).
2. **What the real machine does.** Apple's hardware description (Inside Macintosh's hardware
   chapter, as mirrored in the Linux/m68k "Mac Plus hardware" notes):
   - during the visible part of each line the 68000's RAM accesses alternate with the video's;
   - the sound/disk-speed fetch takes one access per line;
   - this gives an "average RAM access rate of about 6 MHz";
   - **"the ROM ... is always accessed at the full processor rate of 7.83 MHz"**.

   For code that uses the bus on every cycle (move loops, blitters) that means half speed for
   67% of each frame: 0.66 of full speed, a factor of **1.52**. Code with internal cycles (shifts,
   multiplies, branches) loses less, and ROM code (QuickDraw, SANE) much less.
3. **Benchmarks.** Speedometer 3.23 (Gryphel's repackaged copy, md5-checked) under System 7.0.1
   at 4 MB, against Low End Mac's real-hardware Speedometer 3.06 results (Mac Classic = 1.0):

   | | CPU | Graphics | Math |
   | --- | --- | --- | --- |
   | MAME `macplus` | 1.298 | 1.07 | 1.167 |
   | real Plus (LEM) | 0.87 | 0.91 | 0.99 |
   | MAME `macsefd` | 1.300 | 1.07 | 1.070 |
   | real SE (LEM) | 0.98 | 0.98 | 0.99 |
   | MAME `macplus`, `SLOW=1.5` | **0.853** | 0.706 | 0.766 |
   | MAME `macsefd`, `SLOW=1.3` | **0.991** | 0.820 | 0.816 |

   - **CPU** (code in RAM): MAME is 1.49× the real Plus and 1.33× the real SE, which agrees with
     the 1.52 estimate.
   - **Graphics and math** (mostly ROM code): MAME is only about 1.18× (Plus) and 1.09× (SE)
     too fast, as Apple's "ROM at full speed" predicts. So `SLOW` overstates the slowdown of
     QuickDraw-heavy code.
   - Caveats: Speedometer 3.1 changed its test timing, so 3.06 and 3.23 scores are not strictly
     comparable; the real-hardware figures come from one source (Low End Mac); MAME ran System
     7.0.1, LEM ran 7.5.5.
4. **The `SLOW=<factor>` knob** (`emu/run_test.sh`, `emu/scripts/slow.lua`):
   - How it works: MAME's Lua API has no CPU clock control (`set_clock_scale` isn't bound; the
     overclock slider is interactive and needs `-cheat`). So `slow.lua` puts a 30-byte routine at
     the unused vectors $E0 in front of the level-1 interrupt vector. On every vertical-blank
     interrupt it spins for (1 − 1/factor) of a frame's 130,234 cycles, then jumps to the
     original handler. Lua re-installs it every frame if the vector is rewritten. Without `SLOW`
     nothing changes.
   - Measured on the Plus: `tests/cyclecal` gives 5.14 MHz with `SLOW=1.5`, against 7.79 without.
     Endurance (racer, `racer/tests/game.lua` drive scenario) goes from **26.8 to 17.3 fps**.
     `pl_present` 384×288 goes from 10.3 to **15.2 ms** (the bus-bound prediction is 15.3 ms);
     30 Hz pacing still holds.
   - Limits: the time is taken in one 5.5 ms chunk right after each VBL, not spread over the
     scan lines, so throughput is right but timing within a frame is not. No cycles are taken
     while interrupts are masked.

**Recommendation.** Check frame rates with **`SLOW=1.5` on `macplus`** and **`SLOW=1.3` on
`macsefd`**, and make the game hold its target frame rate there. Confidence:
- **High** that MAME is too fast: the 7.79 MHz measurement, and the source explains why.
- **Medium-high** for 1.5 as the factor for our own code running from RAM: the 68000 cycle
  arithmetic and the Speedometer CPU comparison agree to within 2%. Real code with many
  internal cycles may do somewhat better, down to about 1.3 (Apple's "about 6 MHz").
- For QuickDraw/SANE-heavy work the real factor is nearer 1.2 (Plus) and 1.1 (SE), so
  `SLOW=1.5` is pessimistic there.
- To be confirmed on the real Plus: run `tests/cyclecal` there and read the ticks.

## Measured sizes and limits (2026-10-15)

| Item | Value |
| --- | --- |
| Hello app (`HelloPlus.bin`) | MacBinary 7,168 B; resource fork 7,032 B: CODE 1 (runtime) 4,304, CODE 2 (main) 848, DATA 376, CODE 0 40, 6 empty stubs × 80, RELA 216 |
| Hello app, `--mac-single` | 4,161 B of resources (CODE 1 3,777) |
| `FreeMem()` in the hello app | 856,192 B at 1 MB; 4,001,920 B at 4 MB |
| Biggest test app (`SEG_SCALE=4`) | 556 KB MacBinary; CODE 3/4/5 = 112,604 / 113,564 / 281,916 B; all three resident at once on a **1 MB** Plus left 236,588 B free (745,598 before loading them) |
| Floppy budget | about 670 KB for the app (and its data files) on an 800K disk with trimmed System + Narrow Screen |
| Segment load time | the first calls into three segments (28, 28, 70 KB) took 71, 62 and 119 ticks on the Plus from floppy, but 4, 4 and 8 ticks on the SE from its SCSI disk: loading from floppy runs at about **25 KB/s** (a 28 KB segment costs about 1.1 s) |

Practical limits are RAM and floppy, not the compiler: on a 1 MB Plus the app heap is about
850 KB, shared by resident segments, globals, the heap and your buffers; a single segment
must fit in one contiguous heap block. **Const data** (`const` tables, string literals) goes
into **DATA 0**, which is copied into the A5 world at launch and stays resident. For big
tables, either load them from resources/files on demand, or put them into a code segment
so they are loaded and purged with it:
`__attribute__((section(".text.mytable"))) const unsigned short mytable[] = {...};`
(verified: a 40 KB table moved from DATA 0 into CODE 5, read correctly after an UnloadSeg;
access such a table only from code in the same segment: according to Elf2Mac's source, a
reference to a code-section symbol from another segment is routed through the jump table).

## Gotchas

- **hfsutils keeps its "current volume" in `$HOME/.hcwd`**, shared by every process of the
  user. Two agents running `hmount`/`hformat` at the same time can copy files into each
  other's disk images. Give every script that calls hfsutils a private `HOME` (all the
  repo's disk scripts now do).
- **hfsutils changes images it mounts**, even just to list them (it writes the volume's
  modification date). Keep source images read-only (`chmod 444`; hfsutils then mounts them
  read-only) and work on copies.
- **Globals are only word-aligned at run time.** A5 was `$0F6606` at 1 MB, so
  `__attribute__((aligned(4)))` on a global is not honoured. Scan for magic tags in 2-byte
  steps. (Harmless on the 68000 itself, which only needs even addresses.)
- A window covering `screenBits.bounds` can't draw into the menu bar (its visible region
  excludes the top 20 px; the empty menu bar's bottom line stays visible). To use the full
  height, draw straight to the screen (or use the usual hide-the-menu-bar trick of adding its
  rectangle to `GrayRgn`; not tried here).
- The app is also the shell (no Finder on the disk), so `ExitToShell` should relaunch it
  (expected from the boot blocks; not tested).
- Reading low-memory globals directly (`*(long *)0x16A`) makes GCC 16 warn
  `-Warray-bounds` ("array subscript 0 is outside array bounds"): it treats addresses below
  4 KB as null-pointer offsets. `--param=min-pagesize=0` (in the template's flags) fixes it.
- Under System 7, `MemTop` (`$108`) is **not** the top of RAM (it read `$39DAE4` in a 4 MB SE),
  so Lua scripts take the RAM size from MAME's `-ramsize` option instead (see `check.lua`).
- A new GrafPort uses the application font. On the trimmed Plus disks (no FONT resources)
  that falls back to Chicago, but under System 7 it is Geneva (seen in the C++ test's
  screenshots): call `TextFont` explicitly.
- Building Retro68 on macOS 26/arm64: Apple's `bison` 2.3 is too old (Homebrew `bison`
  3.8.2 first on `PATH`), `makeinfo` is missing (Homebrew `texinfo`), and Apple's `gm4` lacks
  `--gnu` (point `M4` at Homebrew's GNU m4). `nproc: command not found` at the start of the
  log is harmless (the script falls back to `sysctl`). The libstdc++ C++20 `std` module
  build fails with "returning to the gate for a mechanical issue"; the build ignores it.

## Tests in this folder

```sh
toolchain/build_all.sh       # builds everything below into toolchain/build/ (about 4 s)
```

| App | Source | Disk | Check | Verified |
| --- | --- | --- | --- | --- |
| HelloPlus | `template/` | `hello_narrow.img` | `template/check.lua` | PASS at 1 MB and 4 MB |
| HelloPlus | `template/` | `hello_512.img` (no Narrow Screen) | `EXPECT_W=512 ... check.lua` | PASS at 1 MB |
| CppTest | `tests/cpp/` | `cpp_disk.img` | `TAG=CPPT ... tests/check_block.lua` | PASS at 1 MB and 4 MB |
| CppTestNoEx | `tests/cpp/` | `cppnoex_disk.img` | same | PASS at 1 MB |
| SegTest | `tests/segments/` | `seg_disk.img` | `TAG=SEGT ... tests/check_block.lua` | PASS at 1 MB and 4 MB |
| SegTest, `-DSEG_SCALE=4` | same | built by hand (`segbig_disk.img`) | same | PASS at 1 MB |
| SegTest, `-DSEG_TABLE=code` | same | built by hand (`segcode_disk.img`) | same | PASS at 1 MB |
| HelloPlus on the SE | `template/` | `hello_se7.hd` | `MACHINE=macsefd ... EXPECT_W=512 ... check.lua` | PASS (System 7.0.1, 4 MB) |
| CppTest on the SE | `tests/cpp/` | `cpp_se7.hd` | `TAG=CPPT` | PASS (System 7.0.1, 4 MB) |
| SegTest on the SE | `tests/segments/` | `seg_se7.hd` | `TAG=SEGT` | PASS (System 7.0.1, 4 MB) |
| MemProbe on the SE | `tests/memprobe/` | `memprobe_se7.hd` (and `--shell`) | `TAG=MEMP CLICKS="70:400:146"` | PASS; numbers in the SE section |
| LibTest (`toolchain/lib`) | `lib/test/` | `libtest_narrow.img`, `libtest_se7.hd` | `lib/test/check_lib.lua` | PASS on the Plus (1 MB, 448 px) and the SE (4 MB, System 7.0.1) |

The two hand-built variants:

```sh
TC=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
cmake -S toolchain/tests/segments -B toolchain/build/segments_big -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DSEG_SCALE=4
cmake --build toolchain/build/segments_big
python3 -I toolchain/template/build_disk.py --app toolchain/build/segments_big/SegTest.bin --out toolchain/build/segbig_disk.img
TAG=SEGT emu/run_test.sh toolchain/build/segbig_disk.img toolchain/tests/check_block.lua 250
# same with -B toolchain/build/segments_code -DSEG_TABLE=code -> segcode_disk.img
```

`tests/segments/gen.py` generates three segments of arithmetic functions plus a 40 KB const
table, with the expected results computed in Python (cross-checked against host clang).

## Rebuilding or updating Retro68

```sh
cd ~/Retro68 && git pull && git submodule update
cd ~/Retro68-build
PATH="/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/texinfo/bin:/opt/homebrew/opt/m4/bin:$PATH" \
M4=/opt/homebrew/opt/m4/bin/m4 ../Retro68/build-toolchain.bash --no-ppc --ninja > build.log 2>&1
```

A full build took 12 min 23 s (wall) on this MacBook and needs about 5 GB of temporary
space in `~/Retro68-build` (gcc-build alone was 4.6 GB). The gcc and binutils build trees
were deleted afterwards (empty `gcc-build/` and `binutils-build/` folders are kept so that
`build-toolchain.bash --skip-thirdparty`, which rebuilds only Retro68's own tools and
libraries, still accepts the tree). Current footprint: `~/Retro68` 2.4 GB,
`~/Retro68-build` 0.73 GB.

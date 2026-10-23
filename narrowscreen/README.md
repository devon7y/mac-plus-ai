# Narrow Screen

A System 6 extension and Control Panel for the Macintosh Plus. It makes the system
treat the screen as a narrower rectangle, so the menu bar, desktop icons, windows and
dialogs stay out of the columns hidden behind the bezel.

## Using it

1. Put the **Narrow Screen** file in the System Folder and restart.
2. Open **Apple menu → Control Panel → Narrow Screen**.
3. Drag the **Left** and **Right** sliders. Two vertical guide lines on the screen show
   where the new edges will be. Move them until both lines are just inside the visible
   part of the glass.
4. Click **Restart**. The new margins apply at startup.

Notes:

- **Left** moves in 16-pixel steps. The 68000 needs the screen's start address to be
  even, so the left edge can only move by whole 16-pixel words. **Right** moves by 1 pixel.
- Setting both to **0** turns it off completely. Use this if the hardware gets fixed.
- **Hold Shift while the Mac starts** to skip it for one boot.
- **Rescue:** if the Mac ever fails to start, boot from the locked System Tools floppy
  and drag Narrow Screen out of the other disk's System Folder.

Known limitations:

- Programs that assume a 512-pixel-wide screen still draw into the hidden edges.
  Programs that ask QuickDraw for the screen size, like the Finder, follow the new
  width. So far only the Finder and the Control Panel have been tested.
- The "Sorry, a system error occurred" box is drawn at a fixed position and can
  extend into the right margin.
- Only active on the Mac Plus ROM. On other Macs it does nothing.

## How it works

On the Plus ROM, QuickDraw's `InitGraf` describes the screen with trap `ScrnBitMap`
(`$A833`). That trap returns the address from the low-memory global `ScrnBase`, the row
size from `ScreenRow`, and a hard-coded 512×342 size. At startup the INIT:

1. paints the margins black,
2. moves `ScrnBase` right by the left margin (so every app's origin and the cursor move
   with it),
3. replaces `ScrnBitMap` with a small routine in the system heap that reports the
   narrower width, and
4. pins the cursor (`CrsrPin`) to the new width.

Every program that starts afterwards (the Finder included) picks up the narrower screen
when it calls `InitGraf`.

The file holds an `INIT` resource (`src/init.s`), a `cdev` Control Panel
(`src/cdev.s`), and the settings resource `'nsSt' -4064` (version, left, right).

## Building and testing

Requirements: `brew install m68k-elf-binutils hfsutils mame`.

```sh
python3 build.py [--left 32] [--right 32]   # → build/Narrow Screen.bin (MacBinary)
                                            #   and ../emu/work/narrow_test.dc42
../emu/run_mac.sh --fresh                   # try it in a window
../emu/run_test.sh <disk> <script.lua> [s]  # headless scripted run (see ../emu/scripts)
```

`../emu/scripts/test_e2e.lua` opens the Control Panel, clicks both sliders, drags a
thumb, restarts, reinserts the disk, and checks the new margins.

**MAME quirk:** MAME's 800K floppy writes can come out corrupted under some timings.
The first boot of a fresh System disk writes catalog flags, and depending on timing that
write could wipe a catalog node. The build therefore starts from a copy of Apple's
System Tools disk that has already been booted once (`emu/disks/system-6.0-booted`).
Real hardware doesn't have this problem.

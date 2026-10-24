# The website: a Macintosh Plus in your browser

**[macintosh.devon7y.com](https://macintosh.devon7y.com)** runs a Macintosh Plus (4 MB, System 6)
inside a 3D model of the compact Mac. Its desktop holds the programs from this repository plus two
classic freeware extras. There are no accounts, ads or tracking: a static site on Cloudflare Pages.

![Dream painting on the 3D Mac in the browser](../docs/images/dream_site.jpg)

## What the page does

- **Emulator.** Mini vMac (Mac Plus model) compiled to WebAssembly: the prebuilt build from
  [Infinite Mac](https://github.com/mihaip/infinite-mac) at commit `f988f537`, built from
  [mihaip/minivmac](https://github.com/mihaip/minivmac) `0bcab1d5`. `build_site.py` downloads both
  files from that commit and checks their SHA-256. The code around the emulator is ours.
  [`src/worker.js`](src/worker.js) runs the emulator in a Web Worker and implements the interface the
  core calls. Input, video, sound and disk insertion pass through `SharedArrayBuffer`s, so the site
  must be cross-origin isolated (COOP `same-origin` + COEP `require-corp` in `_headers`). It also
  sends `Document-Isolation-Policy: isolate-and-require-corp`, which keeps it isolated inside an
  iframe on a page that isn't (devon7y.com embeds it; desktop Chromium 137+ only).
- **Speed.** "Fast" is 4×, and "Real Plus" is 1× for programs that time themselves by CPU speed.
  At 8×, the Plus keyboard path repeats keys. Mini vMac's AutoSlow drops to 1× whenever screen and
  input are quiet, which is exactly when Pixel and Dream are thinking. So while a program (not the
  Finder) runs, the page taps a key the Plus doesn't have four times a second to keep it at 4×.
- **3D Mac** ([`src/view3d.js`](src/view3d.js), three.js 0.186.1). The emulator's canvas is the
  texture of a mesh that hugs the curved CRT glass. Pointer input is mapped back by raycasting that
  same mesh, so a click lands on exactly the Mac pixel under the pointer. Drag to turn the Mac,
  scroll or pinch to zoom; on release the view springs back to the front and keeps your zoom.
- **CRT.** The screen shader draws crisp pixels in the Plus's blue-white phosphor, with glow,
  scanlines once rows are big enough to show them, edge falloff and a vignette. It also adds subtle
  live effects: a ±4% flicker, a soft bright band rolling down every few seconds, grain and phosphor
  afterglow. The light adds onto polished glass that reflects a studio, with a soft shadow where the
  glass meets the bezel. The beige plastic has a triplanar orange-peel texture, and the front badge
  is set in EB Garamond.
- **Keyboard and mouse.** Every key in the model is its own mesh. Real typing presses the matching
  3D keys, and clicking the 3D keys or mouse button types on the Mac. Browser key codes map to the
  Plus keyboard: Control and ⌘ are Command, and Esc is Esc. Optional click sounds for keys and the
  mouse come from CC0 recordings of an Apple M0118 keyboard and M0100 mouse.
- **Sound.** The Mac's 22,255 Hz audio goes through a `SharedArrayBuffer` ring to an AudioWorklet.
  Machine sounds play alongside it: power button, start-up, the running hum, shutdown, floppy
  insert and eject. They're loudest head-on and close up, and fade as you pan or zoom out.
- **Insert Disk.** Drop or pick a disk image (`.dsk`, `.img`, `.image`, `.dc42`) or an archive
  (`.sit`, `.sea`, `.hqx`, `.bin`, `.zip`), or click the 3D floppy slot. Archives are unpacked in the
  browser. [`src/unpack.js`](src/unpack.js) handles MacBinary, BinHex and zip (fflate), and StuffIt
  1.x–5.x goes through a small WASM build of the Rust `stuffit` crate ([`sit_wasm/`](sit_wasm/)).
  [`src/hfs.js`](src/hfs.js) then builds an HFS floppy image around the files, and a 3D floppy
  slides into the drive.
- **Help panel.** It shows the keys for whichever program is in front ([`src/controls.js`](src/controls.js))
  and has volume sliders for the machine and the Mac.

## The Mac's disk

[`build_disk.py`](build_disk.py) builds a 16 MB HFS volume from the projects' current build outputs
(through `emu/games.json`). It puts every program directly on the Finder desktop by editing the
catalog B-tree, because hfsutils can't set Finder flags or icon positions. It also refuses to finish
if anything not cleared for publication turns up on the volume. [`tools/bake.mjs`](tools/bake.mjs)
then boots the disk once in headless Chromium so the Finder builds its icon database, and shuts it
down cleanly.

## Building

```sh
web/build.sh            # disk, first boot, 3D models, sounds, then the static site in web/dist/
python3 web/tools/serve.py --dir web/dist --port 8000   # with the same headers as the live site
node web/tools/run_tests.mjs web/dist chromium <out>    # Playwright tests (needs the browsers)
```

Needs Python 3, Node, `hfsutils`, and Playwright's browsers. `build.sh` installs exact npm versions
into `web/build/`. Several inputs aren't in this repository, because their licences don't allow it
or because they're Apple's:

| Input (path the build expects) | Where it comes from |
| --- | --- |
| `emu/roms/vMac.ROM` | a Mac Plus ROM image (Apple; supply your own) |
| `toolchain/build/demo/demo.hd` | a hard disk with a System 6.0 System Folder (Apple; supply your own). The live site's also carries the Talking Moose |
| `toolchain/build/demo/extras/*.bin` | Glider 4.06 (John Calhoun, freeware) as MacBinary |
| `web/vendor/mac128k_gltf/` | ["Macintosh 128K Computer (1984)"](https://skfb.ly/6SLnE) by Daz, CC BY-NC 4.0 (glTF download) |
| `web/vendor/floppy/` | ["Floppy disk"](https://sketchfab.com/3d-models/floppy-disk-15444a8efd38476f9457acce300edf0c) by drumdorf, CC BY-SA 4.0 |
| `web/vendor/sounds/` | Freesound [680714](https://freesound.org/s/680714/) and [611812](https://freesound.org/s/611812/) (CC0), listed in [`clicks.json`](clicks.json) |
| `web/vendor/sounds_epidemic/` | machine sounds from Epidemic Sound (licensed to the site's owner, not redistributable). Point [`sfx.json`](sfx.json) at your own recordings |

The apps on the disk come from each project's build (see the top-level README). Dream's trained
model isn't in this repository; see [`../dream/README.md`](../dream/README.md).

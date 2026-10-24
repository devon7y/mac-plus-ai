# Third-party material and credits

The code, art, sounds and model weights in this repository are original work under the MIT licence
([`LICENSE`](LICENSE)), except as listed here.

## Included in this repository

| What | Where | Licence |
| --- | --- | --- |
| PyUSB 1.3.1 | `transfer/vendor/` | BSD 3-Clause (its `LICENSE` is beside it) |
| A screenshot of a personal Plants vs. Zombies port (its characters © Electronic Arts / PopCap Games) | `docs/images/pvz_site.jpg` | shown for commentary only; no game code or assets are included |
| The FLUX Kommunity License text (Black Forest Labs) | `dream/LICENSE-FLUX.md`, `web/LICENSE-FLUX.md` | the licence Dream is published under on the website, with the attribution notice and modification statement |

## Data and models used to train the networks

| Network | Trained on | Notes |
| --- | --- | --- |
| **Pixel** (`llm/`) | chats generated with [Qwen2.5-7B-Instruct](https://huggingface.co/Qwen/Qwen2.5-7B-Instruct) (Apache-2.0) | the weights (`llm/train/checkpoints/pixel_v2.pxm`) are included |
| **Doodle** "Finish it" (`paint/`) | Google's [*Quick, Draw!* dataset](https://github.com/googlecreativelab/quickdraw-dataset), CC BY 4.0 | a sketch-rnn-style decoder (Ha & Eck, 2017); the weights are included. *Quick, Draw!* data © Google, CC BY 4.0 |
| **Dream** (`dream/`) | clip art generated with FLUX 3 Image Klein (Black Forest Labs), FLUX Kommunity License | **the weights and the generated pictures are not included** (see [`dream/README.md`](dream/README.md) and [`dream/LICENSE-FLUX.md`](dream/LICENSE-FLUX.md)). Dream's subject list is taken from the *Quick, Draw!* categories |

The networks were trained on the Digital Research Alliance of Canada's clusters.

## Used by the website but not included

`web/build.sh` downloads or expects these. They're credited on the site in the "Credits & Licenses"
file on the Mac's desktop.

| What | Licence |
| --- | --- |
| [Mini vMac](https://www.gryphel.com/c/minivmac/) by Paul C. Pratt and contributors, in the WebAssembly build of [Infinite Mac](https://github.com/mihaip/infinite-mac) by Mihai Parparita (downloaded at build time from commit `f988f537`, source [mihaip/minivmac `0bcab1d5`](https://github.com/mihaip/minivmac)) | GPL v2 (Mini vMac), Apache 2.0 (Infinite Mac) |
| [three.js](https://threejs.org) 0.186.1 and meshoptimizer; [fflate](https://github.com/101arrowz/fflate) 0.8.2; glTF Transform 4.2.1 (build tool) | MIT |
| the [`stuffit`](https://github.com/benletchford/stuffit-rs) crate 0.4.0 by Ben Letchford, compiled to WebAssembly by `web/sit_wasm/` | MIT / Apache 2.0 |
| EB Garamond (the badge on the 3D Mac) | SIL Open Font License 1.1 |
| ["Macintosh 128K Computer (1984)"](https://skfb.ly/6SLnE) by Daz | CC BY-NC 4.0 |
| ["Floppy disk"](https://sketchfab.com/3d-models/floppy-disk-15444a8efd38476f9457acce300edf0c) by [drumdorf](https://sketchfab.com/drumdorf), resized and recompressed for the site | CC BY-SA 4.0 |
| Key and mouse clicks cut from "Typing on Apple Standard Keyboard (ALPS SKCM Orange)" by robni7 ([Freesound 680714](https://freesound.org/s/680714/)) and "Mouses (M0100, G5431, M2706)" by SieuAmThanh ([Freesound 611812](https://freesound.org/s/611812/)) | CC0 |
| Power, hum and floppy-drive sounds | Epidemic Sound, licensed to the site's owner; not redistributable |
| Glider 4.06 by John Calhoun; the Talking Moose 1.21 desk accessory by Steve Halls with MacinTalk 1.31 | freeware, on the website's disk only |
| Macintosh Plus ROM, System 6 | © Apple; not included anywhere in this repository |

## Tools

The Mac programs are built with [Retro68](https://github.com/autc04/Retro68) (GCC for classic 68k
Macs) and tested in [MAME](https://www.mamedev.org/). Disk images are made with
[hfsutils](https://www.mars.org/home/rob/proj/hfs/).

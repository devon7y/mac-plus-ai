# Pseudo-3D road engine (68000, Mac Plus)

Shared by Endurance (`racer/`) and Rail Runner (`runner/`). Copy this folder into your project
and adapt the layouts and patterns. Python generates GNU `as` (MIT syntax) that both
`m68k-elf-as` and Retro68's `m68k-apple-macos-as` accept; it is position-independent.
Endurance uses it from C (Retro68): see `racer/game/main.c` and `racer/build.py`.

## Files

| File | What it is |
| --- | --- |
| `road.py` | Bit-exact Python reference: `RoadConfig`, per-row tables (`build_tables`), pass 1 (`geometry`), pass 2 (`render`), edge-piece tables (`piece_tables`). Render PNGs with it to design the look; verify MAME dumps against it. |
| `gen_road.py` | `gen_road.py OUTDIR --w 384 --h 240 --hy 80` writes `road_engine.s` (code + tables + C wrappers, one file), `road_engine.h` (C API and constants), plus `road_cfg.inc`/`road_kernel.s`/`road_data.s` for pure-assembly use. |
| `sprites.py` | Sprite pipeline: high-res PIL masters → scaled, optionally outlined, dithered, masked, pre-shifted 1-bit sprites; `SpriteBank.emit()` writes `sprites.s` (data) and `sprites.h` (`SprInfo` table + `SPR_*` indices); `sheet()` makes a preview PNG. |

## Model

Playfield W×H (W multiple of 32), horizon row HY; road rows are `y = HY+1..H-1`, indexed
`k = y - HY` (1 = farthest, KB = H-1-HY = nearest). Per row: depth `zrow[k] = ZNEAR*KB/k`,
road half-width `hw = HWB*W*k/KB`, rumble width, lane-marker positions (`build_tables`).

- **Pass 1 (`RoadGeom`)**, bottom row up: finds the track segment at each row's world depth
  `camz + zrow[k]`, accumulates the curve in screen space (`dx += ddx; x += dx`, 16.16), adds the
  camera's lateral offset (linear, fades to 0 at the horizon), clamps the centre and picks one of
  4 pattern sets from the stripe phase `((camz + zrow[k]) >> STRIPE_SHIFT) & 1` and row parity.
  Output: `RoadRow rows[k] = {pattern set ptr, cx*4, marked}`; read `cx4 >> 2` to place sprites.
- **Pass 2 (`RoadDraw`)**, top row down. Every long of a row is written exactly once.
  - Fast path: grass fill | left edge piece | road fill | [marker piece | road fill]×2 | right
    edge piece | grass fill. A piece is two longs from a table built per pattern set (rumble
    strip of width rw at bit offset a: entry `rw*256 + a*8`).
  - Generic path (narrow far rows, any odd case): boundaries one at a time, decoded by one
    table lookup and merged with `p ^ ((p ^ q) & rmask)`.
  - **Buffer rows have `PF_MG` (2) invisible longs on each side** (`PF_RBV` bytes per row), so
    pieces that hang off the screen edge land in the margin and rows still take the fast path.
    Sprites may also overhang into the margin without clipping.
- **`PfCopy`**: buffer → screen with `movem.l`, skipping the margins.

## C API (`road_engine.h`)

```c
typedef struct { unsigned long pat[4];               /* grass, rumble, road, marker (32-bit patterns) */
                 const unsigned char *pl, *pr, *pm;  /* edge piece tables (see road.py piece_tables) */
                 long pad; } PatSet;                 /* 32 bytes */
typedef struct { unsigned long camz; short camx, pad;
                 const long *segp; unsigned long segend;   /* segment {len, ddx} holding camz + ZNEAR */
                 const long *ring, *ringend;              /* segment ring buffer */
                 const PatSet *pats;  /* 4 sets: [phase0 even][phase0 odd][phase1 even][phase1 odd] */
                 void *rows; } RoadState;                 /* rows: 8*(KB+2) bytes (row KB+1 = sentinel) */
typedef struct { const void *pat; short cx4; short marked; } RoadRow;  /* marked: 0, 1, 2 = no fast path */

void RoadGeom(RoadState *rs);
void RoadDraw(RoadState *rs, void *rowK1, long skip);  /* rowK1 = buf + (HY+1)*PF_RBV; skip ignored */
void PfCopy(const void *firstVisibleLong, void *screen, long rows);   /* screen rows 64 bytes apart */
void RowCopy(const void *src, void *screen, long rows, long srcStride);  /* PF_RB bytes/row, movem */
void SprBlit(void *dst, const void *src, long rows, long wpr, long dstSkip);   /* whole sprite rows */
```

Keep `segp/segend` current after moving the camera (`cam_set` in `racer/game/main.c`):
`while camz + ZNEAR >= segend: segp = next(segp); segend += segp[0]`. Build the 4 `PatSet`s and
their piece tables whenever patterns change (`build_sets` in `main.c`: ~43 KB, a few tens of ms).
Rows you want drawn differently (e.g. fog) can be patched after `RoadGeom`: point `pat` at a
`PatSet` with just the 4 patterns and set `marked = 2` (generic path, no pieces needed).

`SprBlit` draws whole sprite rows, `dst = (dst & mask) | image`, a long (two words) at a time:
each row stores word pairs as (mask, mask, image, image) and an odd last word as (mask, image).
Placement and clipping are in `draw_sprite()` in `main.c` (pre-shift index from `x & 15`; rows
clipped to the buffer; sprites overhanging the 64-px margins fall back to a word-by-word C loop).

## Sprites (`sprites.py`)

```python
o, b = sprites.scale(master_LA_image, w, h, outline=True)   # -> opaque, black arrays (+1 px white outline)
bank.add("name", o, b, nshift=16)    # 16 pre-shifts (1 px steps) or 8 (2 px steps, half the memory)
bank.emit("sprites.s", "sprites.h")  # spr_data[] + spr_info[] {w, h, wpr, nshift, off}
```

Masters for Endurance's cars are drawn with PIL at 8× in `racer/art/cars.py`. Endurance's set
(4 player frames, 3 rival bodies + tail lights × 16 sizes, logo) is 138 KB.

## Layouts (adapting for other games)

Two row layouts: plain `G | R | A | R | G` (offsets 0, 1, 6, 7) and marked
`G | R | A | M | A | M | A | R | G` (all 8), chosen per row by `marked`. The generic path is a
list of `(offset index, pattern slot)` fed to the `BOUND` template, the fast path a sequence of
`fillto`/`piece` calls in `gen_kernel`. A rail track (ballast | rail | sleeper gap | rail | ...)
is a different offsets table in `road.build_tables` plus matching slot lists/pieces. Things that
alternate along the track (sleepers, stripes) come from the phase bit choosing pattern sets.

**2026-10-16 changes (for copies of this engine):** `RoadGeom` writes a sentinel record after
row KB, so the rows buffer must be `8*(KB+2)` bytes; `RoadDraw` stops at it (no row counter) and
no longer supports a row skip; decoded bit offsets are `bit*8` (the rmask table is 8 bytes per
entry); new `RowCopy` (strided `movem` copy, used for Endurance's pre-shifted mountain band).

## Measured (MAME Mac Plus, 1 MB, emulated Ticks)

M1 benchmark (`racer/tests/m1_bench.lua`, no sprites), check frames bit-exact with `road.py`:

| Playfield | Full frame | Geometry | Draw | Copy |
| --- | --- | --- | --- | --- |
| 384×224, horizon 75 (final kernel, Endurance's size) | 24.4 ms = **41.0 fps** | 2.8 ms | 14.7 ms | 7.2 ms |
| 384×240, horizon 80 (final kernel) | 26.2 ms = 38.2 fps | 2.9 ms | 15.8 ms | 7.8 ms |
| 384×240 (first kernel, no fast path) | 33.4 ms = 30.0 fps | 3.7 ms | 21.9 ms | 7.6 ms |
| 320×200 (first kernel) | 26.5 ms = 37.8 fps | 3.2 ms | 17.9 ms | 5.4 ms |
| 256×160 (first kernel) | 20.0 ms = 50.1 fps | 2.5 ms | 13.3 ms | 3.7 ms |

**MAME runs the Plus about 1.5× too fast** (video cycle stealing isn't emulated; see
`toolchain/README.md`); use `SLOW=1.5` with `emu/run_test.sh` for real-machine rates. Endurance
(384×224, sprites, HUD, sound, logic): 33–34 fps in MAME, **20.6–21.8 fps with `SLOW=1.5`**.

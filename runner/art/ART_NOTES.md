# Rail Runner art notes

All sprites are original 1-bit pixel art for the Mac Plus (no Subway Surfers likenesses).

## Files

| File | What it is |
| --- | --- |
| `sprites.txt` | The masters the engine reads (format: `tools/sprites.py`). **Generated**, do not hand-edit. |
| `compose.py` | The source: an ASCII part library (heads, torsos, arms, legs, shoes, props) plus the frame layouts. `python3 compose.py` rewrites `sprites.txt`. Every frame of a character is built from the same parts, so proportions stay consistent. The title logo is generated procedurally (`make_logo`). |
| `look.py`, `asc.py`, `mock.py` | Review helpers: 1x and zoomed renders on white and gravel, and a rough in-game mock. |
| `preview_sprites.png` | `python3 tools/sprites.py preview` (every sprite at 3x on white and on gravel). |

## Characters

- **Rex** (seen from behind). Backwards black cap with a white **R** on the front panel, which now faces the camera. The white brim points at the camera, with black hair under it. He wears a long-sleeved white tee with a big black star on the back, black shorts with white side stripes, white legs with a sock stripe, and big white sneakers. He always holds the spray can (black can, white cap and nozzle) in his **right hand (screen right)**. His figure is mostly white with a black outline, so he reads on the grey track and on the white sleepers.
- **Officer Grumble** (from behind, bigger). Peaked police cap (white top, checkered band), dark hair with a V nape, jug ears. Black guard jacket with a white **GUARD** patch, epaulettes and a reflective stripe, a white duty belt with pouches, black trousers and boots. He is mostly black, the opposite of Rex. His arms are drawn with a 1 px white seam where they cross the jacket.
- **Biscuit** (from behind). A small white dog with floppy black ears, a collar, a black spot on one haunch and a wagging tail. One hind paw lifts in each frame.

## Sprite list (w x h, anchor)

Rex anchor convention: **(ax, ay) is the same body-relative ground point in every Rex frame**: the ground under the body centre when he stands. Swapping frames never shifts his body, so the engine places the anchor at (player x, ground y - height). In airborne poses the feet sit above the anchor, so for those frames **ay can be greater than h - 1** (the anchor lies below the sprite).

| Sprite | Size | Anchor | Extent left / right of ax | Gap below feet | Notes |
| --- | --- | --- | --- | --- | --- |
| rex_run0..3 | 32x46, 28x47, 28x46, 32x47 | (16,45) (12,46) (12,45) (16,46) | 16/16 max | 0 | Cycle 0,1,2,3. 0 = left foot down, right sole up. 2 = mirror. 1 and 3 = passing (body 1 px higher). |
| rex_jump | 32x43 | (16,51) | 16/16 | 9 | Knees tucked (both soles), both arms up. |
| rex_fall | 38x49 | (19,50) | 19/19 | 2 | Legs extending, arms up and out. |
| rex_left / rex_right | 31x46 / 37x46 | (17,45) / (14,45) | 17/14, 14/23 | 0 | Upper body sheared into the turn, push-off leg out. |
| rex_stumble | 39x48 | (16,47) | 16/23 | 0 | Arms flailing, leaning, 2 sparkles. |
| rex_crash | 41x46 | (19,54) | 19/22 | 9 | Spread-eagle splat, cap flying off, sparkles. |
| rex_jet0/1 | 32x46 | (16,48) | 16/16 | 3 | Twin-tank jetpack on the back, two flame frames, legs dangling. |
| rex_board0/1 | 44x49 / 44x48 | (24,48) / (24,47) | 24/20 | 0 | Wide stance on a 38 px hoverboard; anchor = bottom of the hover lines (board floats 2 px). Upper body bobs 1 px. |
| rex_roll0/1 | 26x24 | (13,23) | 13/13 | 0 | Ball (cap, star, soles); frame 1 is frame 0 rotated 180 degrees. |
| grumble_run0/1 | 57x67 / 55x68 | (27,66) / (27,67) | 27/30 max | 0 | Fist raised and shaking (with shake marks), other arm swinging, legs alternate. Upper body reads on its own if the legs are cut off by the screen bottom. |
| grumble_grab | 50x65 | (25,64) | 25/25 | 0 | Both arms reaching up and forward, open hands. |
| biscuit_run0/1 | 22x23 | (10,22) / (13,22) | 10/12, 13/9 | 0 | Tail wags right / left. |
| pu_jetpack, pu_sneakers, pu_magnet, pu_mult, pu_board | 24x24 | (12,23) | | | 2 px black bubble ring, white inside, bold black icon (twin rockets, high-top sneaker with speed lines, horseshoe magnet, "2x", hoverboard). |
| font_0..font_9 | 8x11 | (0,0) | | | Bold black digits, transparent counters (draw on white). Every glyph fills the 8 px cell; advance 9 px for a 1 px gap. |
| hud_coin | 10x10 | (0,0) | | | Matches the procedural coin (ring plus vertical bar). |
| hud_board | 14x8 | (0,0) | | | Small hoverboard. |
| title_logo | 298x68 | (0,0) | | | "RAIL RUNNER": chunky italic letters, white face with a dithered lower third, 2 px black outline, 4 px black 3D extrusion, three drips, two sparkles. |
| title_rex | 52x83 | (29,82) | | | Front view: backwards cap (brim poking out behind), cocky half-lidded smirk, can raised with a spray puff, other hand on the hip. Anchor is between his feet. |

Animation suggestions: run cycle about 8 to 10 fps (one frame every 6 or 7 ticks, faster at speed); jet, board and roll pairs swap every 4 ticks; Biscuit swaps every 5 ticks; Grumble every 8.

## Things the engine should know

- Wider than the 32 px run frames: rex_fall (38), rex_right (37), rex_stumble (39), rex_crash (41) and rex_board (44). Use per-frame w and ax, not a fixed box.
- `+` and `:` dithers are sprite-local, so they move with the sprite and do not shimmer against it.
- The masters are designed for **1x** (384 px playfield, scale 1). Under `gen_data.scaled()` at about 0.83, the GUARD text and the cap's R start to break up, though both characters still read. At 0.67 the details go. If the playfield ends up narrower than 384, hand-made reduced masters would look better than auto-scaling; ask the art agent.
- Power-up bubbles still read from 0.5 (12 px) upward. At 0.35 and 0.25 they become small blobs, which is fine for far-away pickups.
- Interior blank rows are impossible in this format (a blank line ends a sprite), so detached bits (sparkles, hover lines, shake marks) always overlap a row that already has pixels.

# Controls for every game

## Before you start

- **Standard keys in every keyboard game:**

  | Key | Does |
  | --- | --- |
  | W A S D | Move |
  | Space | Jump / A / accelerate |
  | Shift | Run / B |
  | Return | Start |
  | Tab | Select |
  | M | Sound on/off |
  | Esc | Quit |

  The old arrow keys (and X / Z) still work as alternates.
- **Quitting:** keyboard games quit with **Esc**. In the mouse apps (Doodle, Pixel, Dream) use **File > Quit**. The MacBook's own ⌘Q quits the whole emulator.
- **Start a game:** `emu/play.sh <name>` (add `--full` for fullscreen). `emu/play.sh` on its own
  lists the names.
- **4 MB programs** (Pixel, Dream, Blockworld) spend about 25 s
  testing memory before the Mac starts up.
- **Mouse in window mode:** click inside the window to give the Mac the mouse; press ⌘Tab to get
  it back.
- **Mac Plus keyboard limit:** it only reports two ordinary keys at once. Shift doesn't count, so
  Shift + D + Space (a running jump) works, but W + D + Space doesn't.

---

## Endurance (`racer`)

| Action | Keys |
| --- | --- |
| Steer | Move the mouse, or A / D |
| Accelerate | Hold the mouse button, or W / Space |
| Brake | S |
| Start a race | Click, or Return |
| Pause | P |
| Sound on/off | M |
| Back to the title, then quit | Esc |

Goal: pass the required number of cars each day (50 on day 1, 70 on day 2, more after that).

## Rail Runner (`runner`)

| Action | Keys |
| --- | --- |
| Change lanes | A / D |
| Jump | W |
| Roll | S |
| Hoverboard | Space |
| Start | Return |
| Sound on/off | M |
| Back to the title, then quit | Esc |

## Doodle (`doodle`)

| Action | How |
| --- | --- |
| Draw | Pick a tool on the left and a pattern along the bottom, then drag inside the frame |
| Clear the canvas | Double-click the eraser, or **Edit > Clear Canvas** |
| Undo, Cut, Copy, Paste | **Edit** menu |
| New, Open, Save (MacPaint files) | **File** menu |
| **Finish it** | Sketch part of something inside the dotted frame, click **Finish it**, type what it is (e.g. "cat"), press Return |
| Stop the AI drawing | Click |
| Remove what the AI drew | **Edit > Undo** |
| Drawing style | **Finish** menu: Tidy / Normal / Wild |
| Quit | **File > Quit** |

The Mac studies your sketch first (about 30 s on a real Plus), then draws the rest. The first
"Finish it" waits about 11 s longer while the model loads.

## Pixel the chatbot (`pixel`, or `pixel_se` for the SE version)

| Action | How |
| --- | --- |
| Talk | Type a message and press Return |
| Type with the mouse (SE version only; it opens by itself) | Click letters, then **Send**; Shift for capitals, Delete, Close. Reopen it with **Edit > Keyboard…** or the **Keys** button |
| Correct a typo | Delete (Backspace) |
| Start a new chat (Pixel forgets everything) | **File > New Chat** |
| About Pixel | Apple menu |
| Quit | **File > Quit** |

Pixel takes about 70 s to wake up, then writes about one word piece every 3–4 s in the emulator.
You can type your next message while it's still replying.

## Narrow Screen (`narrowscreen`)

Use the mouse. Choose **Apple menu > Control Panel**, then the Narrow Screen icon, and drag the
sliders to move the screen's left and right edges in from the bezel.

## Blockworld (`blockworld`, or `blockworld_se` for the SE boot floppy)

| Action | How |
| --- | --- |
| Look | Mouse |
| Move | W A S D (walking into a one-block step climbs it by itself) |
| Jump / swim up | Space |
| Mine / hit | Mouse button |
| Place / use / eat / shoot the bow | E |
| Place against a workbench (instead of opening it) | Shift + E |
| Hotbar slot | 1–9 |
| Inventory and crafting | Tab |
| View distance (Short / Normal / Far) | V |
| View size (Normal / Large / Full screen) | S in the Esc menu |
| Look straight down / up | Move the mouse all the way down / up (the crosshair aims at the block under your feet or over your head) |
| Menu | Esc: Back (Esc), View distance (V), View size (S), How to play (H), New world (N), Starting world (R), Save and quit (Q) |
| New world / back to the starting world | Esc, then N or R, then Y (yes) or N (no) |

Needs 4 MB (a 2.5 MB partition under System 7 or MultiFinder). Starts in a ready-made world in about 9 s. Your world is saved as "Blockworld World" next to the game; keep "Blockworld Default" (the starting world) in the same folder. New world in the Esc menu makes a fresh one in about a minute.


## Dream (`dream`, `dream_1mb`, or `dream_se` for the SE boot floppy)

| Action | How |
| --- | --- |
| Make a picture | Type a subject (e.g. "house") and press Return, or click **Dream** |
| Random subject (one of 220) | **Surprise me**, or Return with an empty box |
| Stop early | Click (it stops after the current step) |
| Keep the picture | **File > Copy Picture**, or **File > Save as MacPaint** |
| Quit | **File > Quit** |

Dream loads its model for about 55 s at start-up. A picture develops from static in 6 steps: about
95 s on a real Plus, about 85 s on a real SE.

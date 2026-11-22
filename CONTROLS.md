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
| Hear replies (when MacinTalk is installed: the website, the SE images) | **File > Speak Replies** (on by default) |
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

## Ask AI (`askai`, or `askai_se`)

| Action | How |
| --- | --- |
| Send a message | Type it, Return (queued while a reply streams) |
| Correct a typo | Delete |
| Stop a reply | ⌘. or **Connection > Stop Reply** |
| Connect / hang up | **Connection > Connect / Disconnect** (⌘K) |
| Pick a model | **Model** menu |
| Start over | **File > New Chat** (⌘N) |
| Scroll back | Scroll bar, or the up/down arrow keys (Option for a page) |
| Relay address, PIN, speed, system prompt | **Connection > Settings…** |
| Talk to the modem directly | **Connection > Modem Terminal** |
| About / Quit | Apple menu / **File > Quit** (⌘Q) |

Needs the relay (`askai/relay/`) running and a Hayes-compatible Wi-Fi modem on the modem port; see
`askai/SETUP.md`. Not on the website (its emulator has no serial port).

## Broadside (`broadside`, or `broadside_se` for the SE 1.44 MB floppy)

| Action | Keys |
| --- | --- |
| Move / turn a ship | Drag it / click it, or R with the pointer on it |
| Random fleet, start | Tab, Return (or the buttons) |
| Fire | Click a square in Enemy waters, or W A S D / arrows to aim and Space |
| Salvo rules | Click one square per ship you have afloat; the salvo fires itself |
| Show the Mac's thinking | T (Options menu) |
| Difficulty | ⌘1 / ⌘2 / ⌘3 (Options: Easy / Normal / Hard) |
| Sound on/off | M |
| New game | ⌘N, or Return after a game |
| Quit | Esc, or **File > Quit** |

## Orbit Duel (`duel`, or `duel_se` for the SE 1.44 MB floppy)

| Action | Keys |
| --- | --- |
| Turn | A / D (or arrows) |
| Thrust | W |
| Fire | Space or Shift (Shift lets you turn, thrust and fire at once on the Plus keyboard) |
| Hyperspace (riskier each time) | S |
| Pause | Return (or P) |
| Sound on/off | M |
| Back to the title, then quit | Esc |

Title: 1 play the computer, 2 two players on one keyboard, 3 AppleTalk game, 4 serial cable game; Tab =
computer level. Two players on one keyboard: Needle = W A S D, fire Space/Option/Command; Wedge = I J K L
(or arrows), fire Shift/Enter. Network games (cables, Chooser): `duel/SETUP.md`.

## Scribble Calc (`calc`, or `calc_se` for the SE 1.44 MB floppy)

| Action | How |
| --- | --- |
| Write | Drag on the paper, left to right, one sum per line; end it with **=** |
| Powers | Write the exponent small and raised right after its number (2³) |
| What the Mac read | The line under the paper; the answer follows the = there and is written by hand on the paper |
| Fix a misread symbol | Click it in the line under the paper, then click the right one (best guesses first) or type it; **Automatic** undoes the fix |
| Erase strokes | **Eraser** button (or E), drag over them; **Pen** (or P) to write again |
| Undo | **Undo** button, Delete, or ⌘Z |
| New line | **Clear** button, Return, or **File > New Line** (⌘N) |
| Copy the answer | **Edit > Copy** (⌘C), or click a line on the tape |
| Quit | Esc, or **File > Quit** |

## Tune (`tune`, or `tune_se` for the SE 1.44 MB floppy)

| Action | Keys / mouse |
| --- | --- |
| Compose (the seed shown) / a new tune | Compose button, ⌘K / Again button, Return, ⌘G |
| Play or stop | Play button, Space, ⌘P; Esc stops |
| Type, key, tempo, fiddle/flute, bass | **Tune**, **Key** and **Play** menus |
| Type a seed to make a tune again | **Tune > Seed…** (⌘E) |
| Save as ABC text / copy the staff | Save button, ⌘S / Copy button, ⌘C |
| Sound on/off | M |
| Quit | **File > Quit** |

## Neural Pinball (`pinball`, or `pinball_se` for the SE floppy)

| Action | Keys |
| --- | --- |
| Left / right flipper | A / D (also Z and /, or ← and →) |
| Both flippers | Shift, or the mouse button |
| Plunger | Hold Space or S, let go to launch (harder pull = stronger) |
| Nudge | W (or Space while the ball is in play); three warnings, then TILT |
| Start a game | Return; while the Mac plays by itself, a flipper key takes over its ball |
| Pause | P |
| Sound on/off | M (speech: File > Speech, when MacinTalk is installed) |
| End the game, then quit | Esc |

Goal: spell T-R-A-I-N for Epochs (bonus ×), knock down the Dropout bank to light LOCK, sink two
balls in the Gradient Descent bowl for Overfit Multiball, then shoot the Backprop ramp for the
Jackpot and the bowl for the Singularity.

## Guess My Doodle (`guess`)

| Action | Keys |
| --- | --- |
| Draw | Mouse, in the box |
| Start / Go / Next | Return or the big button |
| Undo last stroke | ⌘Z or Delete |
| Clear the drawing | ⌘K |
| Give up a word | Skip |
| Sound on/off | M |
| Speak guesses (with MacinTalk) | **File > Speak Guesses** |
| Back to the title, then quit | Esc |

## Tank Duel (`tanks`, or `tanks_se` for the SE 1.44 MB floppy)

| Action | Keys |
| --- | --- |
| Drive / turn the hull | W / S, A / D |
| Aim the turret | Mouse |
| Fire | Click, or Space / Shift |
| Pause | Return or P |
| Show the AI's sensors and plans | Tab (**Options > Show the AI**) |
| Difficulty | **Options**: Easy / Normal / Hard |
| Sound on/off | M |
| Back to the title, then quit | Esc |

On the Plus keyboard (two ordinary keys at once), fire with the mouse or Shift while driving and turning.

## Shuttle (`shuttle`: the Plus Modem disk, with Ask AI)

| Action | Keys |
| --- | --- |
| Connect / Disconnect | ⌘K or the Connect button |
| Open a folder or archive / download a file | double-click, Return, or the Open/Download button (⌘D) |
| Back to the folder above | Up, ⌘↑ or ← |
| Move the selection | ↑ ↓, or type the start of a name |
| Send a file to the MacBook | Send File... (⌘U) |
| Cancel a transfer | Cancel or ⌘. |
| Refresh the list | ⌘R |
| Talk to the modem | **Connection > Modem Terminal** (⌘T) |
| Quit | ⌘Q or the close box |

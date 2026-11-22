// Every program's keys, one row per action: [keys, action], shown in the Help panel (the program in
// front first, by its name in CurApName). Keys are shown as <kbd> chips ("A / D" is two chips). Groups ({group: "..."})
// split long lists (Blockworld). Source of truth: CONTROLS.md, plus Glider 4.06's own defaults. The
// Mac disk's Read Me (build_disk.py) keeps its own copy in the same order.
export const APPS = {
  Endurance: {
    title: "Endurance",
    rows: [
      ["A / D", "Steer (or the mouse)"],
      ["W / Space", "Gas (or the mouse button)"],
      ["S", "Brake"],
      ["Return", "Start a race"],
      ["P", "Pause"],
      ["M", "Sound on/off"],
      ["Esc / `", "Title, then quit"],
    ],
  },
  "Rail Runner": {
    title: "Rail Runner",
    rows: [
      ["A / D", "Change lanes"],
      ["W", "Jump"],
      ["S", "Roll"],
      ["Space", "Hoverboard"],
      ["Return", "Start"],
      ["M", "Sound on/off"],
      ["Esc / `", "Title, then quit"],
    ],
  },
  Blockworld: {
    title: "Blockworld",
    hint: "Starts in a ready-made world. Click the screen to look around with the mouse; Esc gives the mouse back.",
    rows: [
      { group: "Move" },
      ["W A S D", "Walk"],
      ["Space", "Jump / swim up"],
      ["Mouse", "Look"],
      { group: "Act" },
      ["Click", "Mine / hit"],
      ["E", "Place / use / eat / bow"],
      ["Shift E", "Place on a workbench"],
      ["1 – 9", "Hotbar slot"],
      { group: "Menus" },
      ["Tab", "Inventory and crafting"],
      ["V", "View distance"],
      ["Esc / `", "Menu"],
      { group: "In the Esc menu" },
      ["S", "View size"],
      ["N", "New world (then Y)"],
      ["R", "Back to the starting world (then Y)"],
      ["Q", "Save and quit"],
    ],
  },
  "Glider 4.06": {
    title: "Glider 4.06",
    rows: [
      ["⌘ N", "New game (Ctrl N here)"],
      [",", "Fly left"],
      [".", "Fly right"],
      ["Return", "Battery boost"],
      ["Space", "Rubber band"],
      ["Tab", "Pause"],
      ["⌘ Q", "Quit (Ctrl Q here)"],
    ],
  },
  Doodle: {
    title: "Doodle",
    hint: "Sketch part of something in the dotted frame, then Finish it.",
    rows: [
      ["Drag", "Draw with the chosen tool"],
      ["Finish it", "Type what it is, Return"],
      ["Click", "Stop the AI drawing"],
      ["⌘ Z", "Undo (Ctrl Z here)"],
      ["File menu", "Save, Open, Quit"],
    ],
  },
  Pixel: {
    title: "Pixel",
    hint: "A tiny chatbot that reads its replies aloud. It takes a little while to wake up.",
    rows: [
      ["Type", "Your message"],
      ["Return", "Send"],
      ["Delete", "Correct a typo"],
      ["File menu", "New Chat, Speak Replies (on or off), Quit"],
    ],
  },
  Dream: {
    title: "Dream",
    hint: "Paints 64×64 clip art from static in six steps. For non-commercial use (see Credits & Licenses on the Mac).",
    rows: [
      ["Type", "A subject, e.g. house"],
      ["Return", "Dream it"],
      ["Surprise me", "Random subject"],
      ["Click", "Stop after this step"],
      ["File menu", "Copy, Save, Quit"],
    ],
  },
  "Guess My Doodle": {
    title: "Guess My Doodle",
    hint: "Draw the word in 20 seconds; the Mac guesses as you draw.",
    rows: [
      ["Mouse", "Draw in the box"],
      ["Return", "Start / Go / Next"],
      ["Ctrl Z / Delete", "Undo a stroke"],
      ["Ctrl K", "Clear the drawing"],
      ["M", "Sound on/off"],
      ["Esc", "Title, then quit"],
    ],
  },
  Tune: {
    title: "Tune",
    hint: "The Mac composes a folk tune, writes it on the staff and plays it.",
    rows: [
      ["Compose", "Write a tune (Return: a new one)"],
      ["Space", "Play / Stop"],
      ["Tune, Key, Play menus", "Type, key, tempo, fiddle or flute, bass"],
      ["Ctrl S", "Save as ABC text"],
      ["M", "Sound on/off"],
    ],
  },
  "Scribble Calc": {
    title: "Scribble Calc",
    hint: "Write a sum with the mouse and end it with =.",
    rows: [
      ["Mouse", "Write on the paper"],
      ["Click a symbol below", "Fix a misread symbol"],
      ["Delete", "Undo"],
      ["E / P", "Eraser / pen"],
      ["Return", "New line"],
      ["Esc", "Quit"],
    ],
  },
  "Neural Pinball": {
    title: "Neural Pinball",
    hint: "Spell TRAIN, lock two balls, then go for the Jackpot.",
    rows: [
      ["A / D", "Left / right flipper"],
      ["Shift", "Both flippers (or the mouse button)"],
      ["Space / S", "Hold, then let go to launch"],
      ["W", "Nudge (careful: tilt)"],
      ["Return", "Start a game"],
      ["P", "Pause"],
      ["Esc", "End the game, then quit"],
    ],
  },
  "Tank Duel": {
    title: "Tank Duel",
    hint: "Shells bounce once: bank your shots.",
    rows: [
      ["W / S", "Drive"],
      ["A / D", "Turn"],
      ["Mouse", "Aim the turret"],
      ["Space / Shift", "Fire (or click)"],
      ["Tab", "Show the AI"],
      ["P", "Pause"],
      ["Esc", "Title, then quit"],
    ],
  },
  Broadside: {
    title: "Broadside",
    hint: "Sink the Mac's fleet before it sinks yours.",
    rows: [
      ["Drag", "Place a ship (click to turn it)"],
      ["Tab", "Random fleet"],
      ["Return", "Start"],
      ["Click", "Fire"],
      ["T", "Show the Mac's thinking"],
      ["Ctrl 1 / Ctrl 2 / Ctrl 3", "Easy / Normal / Hard"],
      ["Esc", "Quit"],
    ],
  },
  "Orbit Duel": {
    title: "Orbit Duel",
    hint: "Two ships, one star. First to 5 wins.",
    rows: [
      ["A / D", "Turn"],
      ["W", "Thrust"],
      ["Space / Shift", "Fire"],
      ["S", "Hyperspace (riskier each time)"],
      ["1 / 2", "Play the computer / two players"],
      ["Esc", "Title, then quit"],
    ],
  },
  TeachText: {
    title: "TeachText",
    rows: [
      ["Type", "Edit the text"],
      ["⌘ Q", "Quit (Ctrl Q here)"],
    ],
  },
  "Talking Moose": {
    title: "The Talking Moose",
    rows: [
      ["Apple menu", "Talking Moose"],
      ["Close box", "Close its settings window"],
    ],
  },
};

// Render rows as a keybind list: <dl class="keys"> with <kbd> chips.
export function keyList(rows) {
  const dl = document.createElement("dl");
  dl.className = "keylist";
  for (const r of rows) {
    if (!Array.isArray(r)) {
      const g = document.createElement("div");
      g.className = "keygroup";
      g.textContent = r.group;
      dl.append(g);
      continue;
    }
    const [keys, action] = r;
    const dt = document.createElement("dt");
    keys.split(" / ").forEach((alt, i) => {
      if (i) dt.append(document.createTextNode(" / "));
      if (/^[A-Z][a-z]+ ?[a-z]*$/.test(alt) && !["Return", "Space", "Tab", "Shift", "Esc", "Delete"].includes(alt.split(" ")[0])) {
        const s = document.createElement("span"); s.className = "keyword"; s.textContent = alt; dt.append(s);   // "Double-click", "Apple menu"
      } else {
        alt.split(" ").forEach((k, j) => {
          if (j) dt.append(document.createTextNode(" "));
          const kbd = document.createElement("kbd"); kbd.textContent = k; dt.append(kbd);
        });
      }
    });
    const dd = document.createElement("dd");
    dd.textContent = action;
    dl.append(dt, dd);
  }
  return dl;
}

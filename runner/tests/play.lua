-- Rail Runner end-to-end play test (normal build), driven only through MAME's own keyboard input
-- fields (press / hold, no RAM writes); state is read from the 'RRUN' debug block and the sound
-- buffer. Works on macplus and macsefd (keys found by name in every input port).
--   emu/run_test.sh runner/build/game/game.img runner/tests/play.lua 300
--   MACHINE=macsefd emu/run_test.sh runner/build/game/game.img runner/tests/play.lua 300
-- Title: M mutes (sound buffer silent), M again unmutes. Run 1 (standard keys): A -> lane 0,
-- D -> lane 1, W jumps (and its blip plays, so sound is back on), S rolls, Space while D is held
-- -> lane 2 on the hoverboard. Then no input until the crash, game over panel, best score,
-- Return -> title. Run 2 (alternate keys): Left, Right, Up, Down, then Esc -> title; Esc on the
-- title quits (the floppy has no Finder, so the system relaunches the game: runs counter reset).
-- Esc is the SE's Esc key, or the Plus's ` key (what emu/play.sh maps the host's Esc to).
local cpu = manager.machine.devices[":maincpu"]          -- (not scripts/lib.lua: it is Plus-only)
local mem = cpu.spaces["program"]
local screen = manager.machine.screens[":screen"]
local MAGIC = 0x5252554E
local blk, next_scan = nil, 8
local fails = 0
local function check(ok, what)
  print(string.format("CHECK %-4s %s", ok and "ok" or "FAIL", what))
  if not ok then fails = fails + 1 end
end
local function scan()
  local top = mem:read_u32(0x108)
  for a = top - 4, 0x1000, -2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then return a end
  end
end

local ports = manager.machine.ioport.ports
local function find_named(names)
  for _, want in ipairs(names) do
    for _, port in pairs(ports) do
      for name, f in pairs(port.fields) do if name == want then return f end end
    end
  end
end
local function find_char(c)                 -- letter fields are named like "a  A"
  for _, port in pairs(ports) do
    for name, f in pairs(port.fields) do if name:find("^" .. c .. "  ") then return f end end
  end
end
local KEYS = {
  up = find_named({"Cursor Up", "Up Arrow"}), down = find_named({"Cursor Down", "Down Arrow"}),
  left = find_named({"Cursor Left", "Left Arrow"}), right = find_named({"Cursor Right", "Right Arrow"}),
  ret = find_named({"Return"}), space = find_named({"Space"}),
  esc = find_named({"Esc"}) or find_char("`"),   -- the Plus has no Esc: emu/play.sh maps the host's Esc to `
  a = find_char("a"), s = find_char("s"), d = find_char("d"), w = find_char("w"), m = find_char("m"),
}
for k, f in pairs(KEYS) do if not f then print("RESULT FAIL (no input field for key " .. k .. ")"); manager.machine:exit() end end
print(string.format("Esc key field: %s mask %02x \"%s\"", KEYS.esc.port.tag, KEYS.esc.mask, KEYS.esc.name))
local held = {}
local function press(k, t, secs) KEYS[k]:set_value(1); held[k] = t + (secs or 0.12) end
local function rd(o) return mem:read_i32(blk + o) end
local function rd16(o) return mem:read_i16(blk + o) end
local function snd_silent()                 -- the 370 samples (even bytes) all at the midpoint
  local base = mem:read_u32(0x266) & 0xFFFFFF
  for i = 0, 369 do if mem:read_u8(base + 2 * i) ~= 128 then return false end end
  return true
end
local function st() return string.format("lane %d y %d roll %d hover %d boards %d state %d",
  rd16(94), rd16(96), rd16(448), rd16(450), rd16(452), rd16(64)) end

local phase, t_phase = "boot", 0
local best_before, heard = nil, false
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  for k, until_t in pairs(held) do
    if t >= until_t then KEYS[k]:clear_value(); held[k] = nil end
  end
  if not blk then
    if t >= next_scan then blk = scan(); next_scan = t + 4 end
    if t > 150 then print("RESULT FAIL (no block)"); manager.machine:exit() end
    return
  end
  local mode = rd(12)
  local dt = t - t_phase
  if phase == "boot" and mode == 1 then
    print(string.format("title at t=%.1f, pf at (%d,%d), freemem %d, best %d", t, rd16(70), rd16(72), rd(90), rd(114)))
    best_before = rd(114)
    screen:snapshot("play_title.png")
    phase, t_phase = "mute", t + 0.5
  elseif phase == "mute" and t > t_phase then
    check(rd16(120) == 1, "sound on at the title")
    press("m", t); phase, t_phase = "mute_chk", t
  elseif phase == "mute_chk" and dt > 0.3 then
    check(rd16(120) == 0 and snd_silent(), "M mutes (flag off, sound buffer silent)")
    press("m", t); phase, t_phase = "unmute_chk", t
  elseif phase == "unmute_chk" and dt > 0.3 then
    check(rd16(120) == 1, "M again unmutes")
    press("ret", t); phase, t_phase = "wait_play", t
  elseif phase == "wait_play" and mode == 2 then
    check(true, "Return starts a run")
    phase, t_phase = "run1", t
  elseif phase == "run1" then          -- timed steps from the start of the run
    if dt > 0.25 and not held.a and rd16(94) == 1 and not KEYS.a_done then KEYS.a_done = true; press("a", t) end
    if dt > 0.50 and not KEYS.a_chk then KEYS.a_chk = true; check(rd16(94) == 0 and rd16(64) == 0, "A: lane 1 -> 0 (" .. st() .. ")"); press("d", t) end
    if dt > 0.80 and not KEYS.d_chk then KEYS.d_chk = true; check(rd16(94) == 1 and rd16(64) == 0, "D: lane 0 -> 1 (" .. st() .. ")"); press("w", t) end
    if dt > 0.85 and dt < 1.0 and not snd_silent() then heard = true end
    if dt > 0.97 and not KEYS.w_chk then KEYS.w_chk = true; check(rd16(96) > 0 and rd16(64) == 0, "W jumps (" .. st() .. ")")
      check(heard, "the jump blip plays (sound back on after M, M)"); press("s", t) end
    if dt > 1.10 and not KEYS.s_chk then KEYS.s_chk = true; check(rd16(448) > 0 and rd16(64) == 0, "S rolls (" .. st() .. ")") end
    if dt > 1.35 and not KEYS.dh then KEYS.dh = true; press("d", t, 0.6) end            -- hold D ...
    if dt > 1.50 and not KEYS.sp then KEYS.sp = true; press("space", t) end            -- ... and press Space
    if dt > 1.70 and not KEYS.sp_chk then KEYS.sp_chk = true
      check(held.d ~= nil, "D still held when Space was checked")
      check(rd16(94) == 2 and rd16(450) > 0 and rd16(452) == 0 and rd16(64) == 0,
        "Space while holding D: lane 2 on the hoverboard (" .. st() .. ")")
      screen:snapshot("play_board.png")
      phase, t_phase = "run", t
    end
  elseif phase == "run" then
    if mode == 3 then
      print(string.format("game over at t=%.1f: score %d coins %d state %d frames %d", t, rd(52), rd(56), rd16(64), rd(16)))
      check(rd(114) >= rd(52), "best score updated")
      screen:snapshot("play_over.png")
      phase, t_phase = "over", t
    elseif dt > 150 then
      check(false, "run ends within 150 s with no input"); phase = "done"
    end
  elseif phase == "over" and dt > 1.5 then
    press("ret", t); phase, t_phase = "back", t
  elseif phase == "back" and dt > 1.0 then
    check(mode == 1, "Return goes back to the title")
    screen:snapshot("play_title2.png")
    press("ret", t); phase, t_phase = "wait_play2", t
  elseif phase == "wait_play2" and mode == 2 then
    phase, t_phase = "run2", t
  elseif phase == "run2" then          -- alternate keys; each check passes once its state shows up
    -- (within 0.35 s: the Plus keyboard sends an arrow key as two messages, a little later than a letter)
    local steps = {
      {0.25, "left", function() return rd16(94) == 0 end, "Left arrow: lane 1 -> 0"},
      {0.65, "right", function() return rd16(94) == 1 end, "Right arrow: lane 0 -> 1"},
      {1.05, "up", function() return rd16(96) > 0 end, "Up arrow jumps"},
      {1.20, "down", function() return rd16(448) > 0 end, "Down arrow rolls"},
    }
    for i, sp in ipairs(steps) do
      local k = "step" .. i
      if dt > sp[1] and not KEYS[k] then KEYS[k] = "pressed"; press(sp[2], t) end
      if KEYS[k] == "pressed" and sp[3]() and rd16(64) == 0 then KEYS[k] = "ok"; check(true, sp[4] .. " (" .. st() .. ")") end
      if KEYS[k] == "pressed" and dt > sp[1] + 0.35 then KEYS[k] = "fail"; check(false, sp[4] .. " (" .. st() .. ")") end
    end
    if dt > 1.60 and not KEYS.esc2 then KEYS.esc2 = true; press("esc", t) end
    if dt > 2.10 then check(mode == 1, "Esc goes back to the title"); phase, t_phase = "quit", t end
  elseif phase == "quit" and dt > 1.0 then
    best_before = rd(114)
    check(rd16(118) == 2, "two runs so far (runs counter " .. rd16(118) .. ")")
    press("esc", t); phase, t_phase = "quit_chk", t
  elseif phase == "quit_chk" and dt > 8.0 then
    -- the game floppy has no Finder, so quitting relaunches Rail Runner: a fresh debug block
    local b = scan()
    if b then blk = b end
    check(b ~= nil and rd(12) == 1 and rd16(118) == 0 and rd(114) == best_before,
      string.format("Esc on the title quits (relaunched: mode %d, runs %d, best %d kept)", rd(12), rd16(118), rd(114)))
    screen:snapshot("play_quit.png")
    phase = "done"
  end
  if phase == "done" or t > 290 then
    print("RESULT " .. (fails == 0 and phase == "done" and "PASS" or ("FAIL (" .. fails .. ", phase " .. phase .. ")")))
    manager.machine:exit()
  end
end)

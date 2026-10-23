-- Blockworld: how long the menu's Yes / No box takes to appear. In the first game and again in the
-- game restarted after Esc, Q (Plus floppy): Esc (the menu), R (Starting world), then each frame a
-- hash of the screen's middle rows; prints when it changes after the key (the last change: the box
-- is drawn) and takes screenshots at +0.3, +0.6, +1.0, +2.0 s (shots/cd_<session>_<ms>.png); then
-- N (No), Esc. Plus floppy only (the SE floppy quits to the Finder).
--   RAM=4M emu/run_test.sh blockworld/build/blockworld.img blockworld/tests/confirmdraw.lua 400
local cpu = manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local screen = manager.machine.screens[":screen"]
local MAGIC, NOTMAGIC = 0x4257474D, 0xBDA8B8B2
local ports = manager.machine.ioport.ports
local function find_named(names)
  for _, want in ipairs(names) do
    for _, port in pairs(ports) do
      for name, f in pairs(port.fields) do if name == want then return f end end
    end
  end
end
local function find_char(c)
  for _, port in pairs(ports) do
    for name, f in pairs(port.fields) do if name:find("^" .. c .. "  ") then return f end end
  end
end
local KEYS = {r = find_char("r"), n = find_char("n"), q = find_char("q"), esc = find_named({"Esc"}) or find_char("`")}
local function scan()
  for a = 0x1000, mem:read_u32(0x108) - 8, 2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == NOTMAGIC then return a end
  end
end
local function hash()                       -- rows 60-239 of the screen
  local base, h = mem:read_u32(0x824), 0
  for a = base + 60 * 64, base + 240 * 64 - 4, 4 do h = (h * 31 + mem:read_u32(a)) % 4294967291 end
  return h
end
local blk, next_scan, session, phase, t0, held = nil, 30, 1, "wait", 0, {}
local last_h, changes, shots
local function press(k, t) KEYS[k]:set_value(1); held[k] = t + 0.25 end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  for k, until_t in pairs(held) do if t >= until_t then KEYS[k]:clear_value(); held[k] = nil end end
  if not blk then
    if t >= next_scan then blk = scan(); next_scan = t + 0.5 end
    if t > 380 then print("RESULT FAIL no game"); manager.machine:exit() end
    return
  end
  local ready = mem:read_u32(blk + 8) == 2 and mem:read_u32(blk + 40) >= 1
  if phase == "wait" then
    if ready then phase = "settle"; t0 = t; mem:write_u16(blk + 214, 1) end
  elseif phase == "settle" and t - t0 > 2 then press("esc", t); phase = "menu"; t0 = t
  elseif phase == "menu" then
    if mem:read_i16(blk + 170) == 9 and t - t0 > 1.5 then
      mem:write_u16(blk + 212, 0); last_h = hash(); press("r", t); phase = "key"; t0 = t
    elseif t - t0 > 4 then press("esc", t); t0 = t end
  elseif phase == "key" then
    if mem:read_u16(blk + 212) == 0x10F then
      print(string.format("session %d: R seen %.3f s after the press", session, t - t0))
      phase = "watch"; t0 = t; changes = {}; shots = {0.3, 0.6, 1.0, 2.0}
    elseif t - t0 > 3 then mem:write_u16(blk + 212, 0); press("r", t); t0 = t end
  elseif phase == "watch" then
    local h = hash()
    if h ~= last_h then changes[#changes + 1] = t - t0; last_h = h end
    if shots[1] and t - t0 >= shots[1] then
      screen:snapshot(string.format("cd_%d_%04d.png", session, math.floor(shots[1] * 1000))); table.remove(shots, 1)
    end
    if t - t0 > 3 then
      local s = {}
      for _, c in ipairs(changes) do s[#s + 1] = string.format("%.2f", c) end
      print(string.format("RESULT session %d: the screen changed at +%s s (the box done at +%.2f s)", session,
        table.concat(s, ", +"), changes[#changes] or -1))
      press("n", t); phase = "no"; t0 = t
    end
  elseif phase == "no" and t - t0 > 1.5 then press("esc", t); phase = "back"; t0 = t
  elseif phase == "back" and t - t0 > 1.5 then
    if session == 2 then manager.machine:exit(); return end
    press("esc", t); phase = "menu2"; t0 = t
  elseif phase == "menu2" and t - t0 > 1.5 then
    if mem:read_i16(blk + 170) == 9 then press("q", t); phase = "quit"; t0 = t
    else press("esc", t); t0 = t end
  elseif phase == "quit" and t - t0 > 1 then
    mem:write_u32(blk, 0); blk = nil; next_scan = t + 1; session = 2; phase = "wait"
  end
end)

-- Blockworld M7: the shipped starting world, the menu's New world and Starting world, saves.
--   1. The first start loads "Blockworld Default" (no world yet): its load time, a screenshot.
--   2. Esc, N (New world), N (No): the same world. Esc, N, Y: a new world (the progress bar timed
--      and photographed), saved. Esc, Q: saved and quit; the restarted game continues it.
--   3. Esc, R (Starting world), Y: the shipped world again, saved; Esc, Q: the restart continues it.
-- Works on the Plus (RAM=4M) and the SE (MACHINE=macsefd RAM=4M). Screenshots in $RUN_DIR/shots.
--   RAM=4M [SLOW=1.5] emu/run_test.sh blockworld/build/blockworld.img blockworld/tests/world.lua 600
local cpu = manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local screen = manager.machine.screens[":screen"]
local MAGIC, NOTMAGIC = 0x4257474D, 0xBDA8B8B2
local blk, last_blk, next_scan, fails = nil, nil, 30, 0
local function check(ok, what)
  print(string.format("CHECK %-4s %s", ok and "ok" or "FAIL", what))
  if not ok then fails = fails + 1 end
end
local function ram_end()                  -- (MemTop at $108 is not the top of RAM under System 7)
  local n, u = manager.options.entries["ramsize"]:value():match("(%d+)(%a?)")
  n = tonumber(n) * ((u == "M" or u == "m") and 1048576 or ((u == "K" or u == "k") and 1024 or 1))
  return math.max(mem:read_u32(0x108), n)
end
local function scan()
  if last_blk and mem:read_u32(last_blk) == MAGIC and mem:read_u32(last_blk + 4) == NOTMAGIC then return last_blk end
  for a = 0x1000, ram_end() - 8, 2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == NOTMAGIC then return a end
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
local function find_char(c)
  for _, port in pairs(ports) do
    for name, f in pairs(port.fields) do if name:find("^" .. c .. "  ") then return f end end
  end
end
local KEYS = { n = find_char("n"), y = find_char("y"), r = find_char("r"), q = find_char("q"),
               esc = find_named({"Esc"}) or find_char("`") }
for k, f in pairs(KEYS) do if not f then print("RESULT FAIL (no input field for key " .. k .. ")"); manager.machine:exit() end end
local held = {}
local function press(k, t) KEYS[k]:set_value(1); held[k] = t + 0.3 end
local function rd(o) return mem:read_i32(blk + o) end
local function rdu(o) return mem:read_u32(blk + o) end
local function rd16(o) return mem:read_i16(blk + o) end
local function s(ticks) return ticks / 60.15 end
local function shot(name) screen:snapshot(name .. ".png"); print("  shot " .. name) end

-- the plan: {action, then wait: seconds or a function of t that says when to go on (60 s at most)}
local plan, pi, t_go = {}, 1, 0
local function step(f, w) plan[#plan + 1] = {f, w or 0.3} end
local S = {}
local function ready() return blk and rdu(8) == 2 and rdu(40) >= 1 end
local function relaunch(t)                -- after Q: forget this game's block, find the next one
  mem:write_u32(blk, 0)
  blk = nil; next_scan = t + 0.5; S.tq = t
end
local function app_name()
  local n, a = mem:read_u8(0x910), ""
  for i = 1, math.min(n, 31) do a = a .. string.char(mem:read_u8(0x910 + i)) end
  return a
end
local K = {n = 0x2D, y = 0x10, r = 0x0F, q = 0x0C}
-- Esc until the menu is up (panel 9); a key the menu (0x100) or the Yes / No box (0x200) has seen
local function open_menu()
  step(function(t) S.tk = t; press("esc", t) end, function(t)
    if rd16(170) == 9 then return true end
    if t - S.tk > 2.5 then S.tk = t; press("esc", t) end
  end)
  step(function(t) end, 0.8)
end
local function key_seen(k, where)
  step(function(t) S.tk = t; mem:write_u16(blk + 212, 0); press(k, t) end, function(t)
    if mem:read_u16(blk + 212) == (K[k] | where) then return true end
    if t - S.tk > 2.5 then S.tk = t; press(k, t) end
  end)
end

-- 1. the first start
step(function(t)
  S.seed0, S.loaded0 = rd(292), rd16(152)
  print(string.format("  t=%.1f first frame: loaded %d (1 the save, 2 the starting world), seed %d; load %.2f s (Mac ticks: read %.2f, unpack %.2f, world %.2f)",
    t, S.loaded0, S.seed0, s(rdu(158)), s(rdu(274)), s(rdu(278)), s(rdu(282))))
  if S.t_main then print(string.format("RESULT first start: from the game's start to its first frame %.2f s (emulated)", t - S.t_main)) end
  check(S.loaded0 == 2, "the first start loads the shipped starting world")
  S.aim0 = rd16(270)
  mem:write_u16(blk + 214, 1)             -- (no natural spawning: steady screenshots)
  shot("first_world")
end, 1.0)
-- 2. New world: No, then Yes
open_menu()
step(function(t) shot("menu") end, 0.2)
key_seen("n", 0x100)
step(function(t) end, 1.0)
step(function(t) shot("confirm_new") end, 0.2)
key_seen("n", 0x200)
step(function(t) end, 1.0)
step(function(t) shot("menu_after_no"); check(rd16(170) == 9, "No: back in the menu"); S.tk = t; S.tries = 1; press("esc", t) end,
  function(t)
    if rd16(170) == -1 then
      print(string.format("  Esc closed the menu after %d tries", S.tries)); return true
    end
    if t - S.tk > 2.5 then
      print(string.format("  Esc not seen: last key %x, KeyMap byte 6 %02x", mem:read_u16(blk + 212), mem:read_u8(0x17A)))
      S.tk = t; S.tries = S.tries + 1; press("esc", t)
    end
  end)
step(function(t)
  check(rd(292) == S.seed0 and rd16(296) == 0, string.format("No keeps the world (seed %d, new worlds %d)", rd(292), rd16(296)))
  check(rd16(270) == S.aim0, string.format("closing the menu keeps the view (aim %d, was %d)", rd16(270), S.aim0))
end, 1.0)
open_menu()
key_seen("n", 0x100)
step(function(t) end, 1.0)
step(function(t) S.tgen = t; S.prog = -100; press("y", t) end, function(t)
  if rdu(8) == 1 then
    local p = mem:read_i16(blk + 312)
    if p >= S.prog + 50 or (p >= 830 and S.prog < 830) then
      print(string.format("  t=%.1f (+%.1f s) progress %d", t, t - S.tgen, p)); S.prog = p
      if p >= 400 and not S.pshot then shot("progress"); S.pshot = true end
    end
  elseif t - S.tgen > 3 and rdu(8) == 2 and rd16(296) < 1 and t - (S.ty or S.tgen) > 3 then
    S.ty = t; press("y", t)                -- (the Yes was missed: again)
  end
  return rd16(296) >= 1 and rdu(8) == 2
end)
step(function(t)
  S.seed1 = rd(292)
  print(string.format("RESULT new world: seed %d, made in %.1f s (Mac ticks; %.1f s emulated with the save), saved in %.2f s (pack %.2f, LZ %.2f, write %.2f; %d tries), error %d",
    S.seed1, s(rdu(12)), t - S.tgen, s(rdu(286)), s(rdu(300)), s(rdu(304)), s(rdu(308)), rd16(318), rd16(156)))
  check(S.seed1 ~= S.seed0 and rd16(156) == 0, string.format("Yes makes a new world (seed %d, was %d) and saves it", S.seed1, S.seed0))
end, function(t) return rdu(40) >= 1 and t - S.tgen > 3 end)
step(function(t) end, 1.5)                -- (the game's loop has copied the new position by then)
step(function(t)
  shot("new_world"); S.x1, S.z1 = rd(16), rd(24)
  check(rd16(270) == 0, string.format("the new world starts looking ahead (aim %d)", rd16(270)))
end, 1.0)
open_menu()
key_seen("q", 0x100)
step(function(t) relaunch(t) end, function(t) return ready() end)
step(function(t)
  print(string.format("  t=%.1f restarted (%.1f s after Q): loaded %d, seed %d, load %.2f s (read %.2f, unpack %.2f, world %.2f)",
    t, t - S.tq, rd16(152), rd(292), s(rdu(158)), s(rdu(274)), s(rdu(278)), s(rdu(282))))
  check(rd16(152) == 1 and rd(292) == S.seed1 and rd(16) == S.x1 and rd(24) == S.z1,
    string.format("save and quit, then a restart: the new world, the player where it was (%.2f, %.2f; was %.2f, %.2f)",
      rd(16) / 256, rd(24) / 256, S.x1 / 256, S.z1 / 256))
  mem:write_u16(blk + 214, 1)
  shot("relaunched")
end, 1.0)
-- 3. the starting world again
open_menu()
key_seen("r", 0x100)
step(function(t) end, 1.0)
step(function(t) shot("confirm_start"); S.tr = t; press("y", t) end, function(t)
  if t - S.tr > 3 and rdu(8) == 2 and rd16(296) < 1 and t - (S.ty or S.tr) > 3 then S.ty = t; press("y", t) end
  return rd16(296) >= 1 and rdu(8) == 2 and rdu(40) >= 1
end)
step(function(t)
  print(string.format("RESULT starting world again in %.1f s (emulated, with the save; load %.2f s)", t - S.tr, s(rdu(158))))
  check(rd(292) == S.seed0, string.format("Starting world: back to seed %d (%d)", S.seed0, rd(292)))
end, 1.5)
step(function(t) shot("starting_again"); S.x2, S.z2 = rd(16), rd(24) end, 1.0)
open_menu()
key_seen("q", 0x100)
step(function(t) relaunch(t) end, function(t) return ready() end)
step(function(t)
  check(rd16(152) == 1 and rd(292) == S.seed0 and rd(16) == S.x2 and rd(24) == S.z2,
    string.format("a restart continues the starting world (%.2f, %.2f; was %.2f, %.2f)", rd(16) / 256, rd(24) / 256, S.x2 / 256, S.z2 / 256))
  shot("relaunched2")
  print(string.format("RESULT %s (%d failed checks)", fails == 0 and "PASS" or "FAIL", fails))
  manager.machine:exit()
end, 0)

local last_step
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  for k, until_t in pairs(held) do
    if t >= until_t then KEYS[k]:clear_value(); held[k] = nil end
  end
  if not blk then
    if S.tq and t - S.tq > 20 and app_name() == "Finder" then    -- (the SE floppy quits to the Finder:
      print("  quit to the Finder (the SE floppy's shell): finder.lua starts the game again from there")
      print(string.format("RESULT %s (%d failed checks; the rest is for a disk that restarts the game)", fails == 0 and "PASS" or "FAIL", fails))
      manager.machine:exit(); return
    end
    if t >= next_scan then
      blk = scan(); next_scan = t + 0.5
      if blk then last_blk = blk; print(string.format("BWGM at %06x (t=%.1f)", blk, t)) end
    end
    if t > 900 then print("RESULT FAIL no debug block"); manager.machine:exit() end
    return
  end
  local stp = mem:read_i16(blk + 290)
  if stp ~= last_step then
    if stp == 1 then S.t_main = t end
    last_step = stp
  end
  if pi > #plan then return end
  if pi == 1 and not ready() then return end
  local w = plan[pi][2]
  if type(w) == "function" then
    if not S.started then plan[pi][1](t); S.started = true; S.tstep = t end
    if blk and w(t) then pi = pi + 1; S.started = false; t_go = t
    elseif t - S.tstep > 150 then
      shot("stuck")
      print(string.format("RESULT FAIL step %d waited %.0f s (panel %d, state %x, last key %x)", pi, t - S.tstep,
        blk and rd16(170) or -9, blk and rdu(8) or 0, blk and mem:read_u16(blk + 212) or 0))
      manager.machine:exit()
    end
    return
  end
  if t < t_go then return end
  plan[pi][1](t)
  t_go = t + w
  pi = pi + 1
end)

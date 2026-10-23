-- Blockworld M7: do MAME's SE floppy writes fail for the Finder too? Boots the SE floppy, quits the
-- game (Esc, Q; prints the save's error and tries), opens the System Folder in the Finder and
-- duplicates "Blockworld Default" (34 KB) N times (default 6) with Command-D, printing the volume's
-- free space after each (it drops by the file's size when the copy worked) and a screenshot
-- (shots/dup_N.png: an error alert shows there). STARTFINDER=1: a disk that starts the Finder (no game).
--   MACHINE=macsefd RAM=4M emu/run_test.sh blockworld/build/blockworld_se.img blockworld/tests/finderdup.lua 600
dofile(os.getenv("EMU") .. "/../toolchain/se7/se_lib.lua")
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
local KEYS = {q = find_char("q"), d = find_char("d"), esc = find_named({"Esc"}) or find_char("`"),
  cmd = find_named({"Command", "Command / Open Apple", "Left Command"})}
for k, f in pairs(KEYS) do if not f then print("RESULT FAIL no key " .. k); manager.machine:exit() end end
local N = tonumber(os.getenv("N") or "6")
local ITEMS = {}                           -- ITEMS="h,v;h,v": duplicate these icons instead (one each)
for h, v in (os.getenv("ITEMS") or ""):gmatch("(%d+),(%d+)") do ITEMS[#ITEMS + 1] = {tonumber(h), tonumber(v)} end
if #ITEMS > 0 then N = #ITEMS end
local function free_bytes()                -- the first volume's free space (VCB +28 vcbAlBlkSiz, +42 vcbFreeBks)
  local vcb = mem:read_u32(0x356 + 2)
  if vcb == 0 then return -1 end
  return mem:read_u32(vcb + 28) * mem:read_u16(vcb + 42)
end
local function scan()
  for a = 0x1000, mem:read_u32(0x108) - 8, 2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == NOTMAGIC then return a end
  end
end
local blk, next_scan = nil, 30
local function dclick(h, v, steps, t0)       -- a double click as timed steps from t0
  steps[#steps + 1] = {t0, function() mouse_to(h, v) end}
  steps[#steps + 1] = {t0 + 0.1, function() btn:set_value(1) end}
  steps[#steps + 1] = {t0 + 0.18, function() btn:clear_value() end}
  steps[#steps + 1] = {t0 + 0.28, function() btn:set_value(1) end}
  steps[#steps + 1] = {t0 + 0.36, function() btn:clear_value() end}
end
local phase, t0, plan, pi, free0, last, ok = "boot", 0, nil, 1, nil, nil, 0
local function make_plan(t)
  local s = {}
  dclick(472, 45, s, t)                      -- the disk
  dclick(103, 110, s, t + 8)                 -- its System Folder
  local tt = t + 16
  s[#s + 1] = {tt, function() free0 = free_bytes(); last = free0; print(string.format("  free %d bytes", free0)); screen:snapshot("dup_0.png") end}
  for i = 1, N do
    local ti = tt + 1 + (i - 1) * 50
    local it = ITEMS[i] or {185, 125}
    s[#s + 1] = {ti, function() mouse_to(it[1], it[2]) end}
    s[#s + 1] = {ti + 0.1, function() btn:set_value(1) end}
    s[#s + 1] = {ti + 0.2, function() btn:clear_value() end}
    s[#s + 1] = {ti + 0.8, function() KEYS.cmd:set_value(1) end}
    s[#s + 1] = {ti + 0.9, function() KEYS.d:set_value(1) end}
    s[#s + 1] = {ti + 1.1, function() KEYS.d:clear_value() end}
    s[#s + 1] = {ti + 1.2, function() KEYS.cmd:clear_value() end}
    s[#s + 1] = {ti + 48, function(t)
      local f = free_bytes()
      local worked = last - f >= 30000
      if worked then ok = ok + 1 end
      print(string.format("t=%.1f duplicate %d: free %d (-%d bytes): %s", t, i, f, last - f, worked and "copied" or "NOT copied"))
      last = f
      screen:snapshot(string.format("dup_%d.png", i))
    end}
  end
  s[#s + 1] = {tt + 1 + N * 50, function()
    print(string.format("RESULT %d of %d Finder duplicates written", ok, N)); manager.machine:exit()
  end}
  return s
end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if phase == "boot" and os.getenv("STARTFINDER") then     -- (a disk that starts the Finder: no game)
    if app_name() == "Finder" and t > 40 then
      print(string.format("t=%.1f the Finder is up (no game first)", t)); phase = "finder"; plan = make_plan(t + 20)
    elseif t > 250 then print("RESULT FAIL no Finder"); manager.machine:exit() end
  elseif phase == "boot" then
    if not blk and t >= next_scan then blk = scan(); next_scan = t + 1 end
    if blk and mem:read_u32(blk + 8) == 2 and mem:read_u32(blk + 40) >= 1 then
      print(string.format("t=%.1f the game is up; Esc, Q", t))
      KEYS.esc:set_value(1); phase = "esc"; t0 = t
    end
    if t > 250 then print("RESULT FAIL no game"); manager.machine:exit() end
  elseif phase == "esc" and t - t0 > 0.3 then KEYS.esc:clear_value(); phase = "menu"; t0 = t
  elseif phase == "menu" and t - t0 > 1.5 then KEYS.q:set_value(1); phase = "q"; t0 = t
  elseif phase == "q" and t - t0 > 0.3 then KEYS.q:clear_value(); phase = "quit"; t0 = t
  elseif phase == "quit" then
    if app_name() == "Finder" and t - t0 > 2 then
      print(string.format("t=%.1f the Finder is up (the save at quit: error %d, %d tries)", t,
        mem:read_i16(blk + 156), mem:read_i16(blk + 318)))
      phase = "finder"; plan = make_plan(t + 20)
    elseif t - t0 > 120 then print("RESULT FAIL no Finder"); manager.machine:exit() end
  elseif phase == "finder" then
    while pi <= #plan and t >= plan[pi][1] do plan[pi][2](t); pi = pi + 1 end
  end
end)

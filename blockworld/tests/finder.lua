-- Blockworld: the Finder icons. Boots a Blockworld disk that has the Finder (the SE floppy), quits
-- the game with Esc, Q (which saves "Blockworld World" next to it), waits for the Finder, then
-- double-clicks each point of CLICKS="h,v;h,v" (global screen coordinates) with a screenshot after
-- each (shots/finder_N.png). NEWWORLD=1 makes a new world first (debug command 11); after the
-- clicks (the last one may start the game again) it waits for the game and prints which world it
-- loaded (1: "Blockworld World") and its seed. Works on macplus and macsefd.
--   MACHINE=macsefd RAM=4M CLICKS="472,52;60,90" emu/run_test.sh blockworld/build/blockworld_se.img blockworld/tests/finder.lua 300
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
local KEYS = {q = find_char("q"), esc = find_named({"Esc"}) or find_char("`")}
local clicks = {}
for h, v in (os.getenv("CLICKS") or ""):gmatch("(%d+),(%d+)") do clicks[#clicks + 1] = {tonumber(h), tonumber(v)} end
local blk, next_scan, phase, t0, n, ci = nil, 30, "boot", 0, 0, 1
local made, asked, old_blk
local function scan()                     -- (the first block not marked as an earlier game's)
  for a = 0x1000, mem:read_u32(0x108) - 8, 2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == NOTMAGIC and mem:read_u16(a + 214) ~= 0x5A5A then return a end
  end
end
local function shot() n = n + 1; screen:snapshot(string.format("finder_%d.png", n)); print(string.format("  shot finder_%d", n)) end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if phase == "boot" then
    if not blk and t >= next_scan then blk = scan(); next_scan = t + 1 end
    if blk and mem:read_u32(blk + 8) == 2 and mem:read_u32(blk + 40) >= 1 then
      if os.getenv("NEWWORLD") and not made then
        if mem:read_u16(blk + 162) == 0 and not asked then
          mem:write_u16(blk + 164, 0); mem:write_u16(blk + 162, 11); asked = true
          print(string.format("t=%.1f the game is up (seed %d): a new world", t, mem:read_i32(blk + 292)))
        elseif asked and mem:read_i16(blk + 296) >= 1 then
          made = mem:read_i32(blk + 292)
          print(string.format("t=%.1f new world %d, saved (error %d, %d tries)", t, made, mem:read_i16(blk + 156), mem:read_i16(blk + 318)))
        end
        return
      end
      print(string.format("t=%.1f the game is up (seed %d); Esc, Q", t, mem:read_i32(blk + 292)))
      mem:write_u16(blk + 214, 0x5A5A)        -- (marks this game's block: a restarted game has its own)
      old_blk = blk
      KEYS.esc:set_value(1); phase = "esc"; t0 = t
    end
    if t > 250 then print("RESULT FAIL no game"); manager.machine:exit() end
  elseif phase == "esc" and t - t0 > 0.3 then
    KEYS.esc:clear_value(); phase = "menu"; t0 = t
  elseif phase == "menu" and t - t0 > 1.5 then
    KEYS.q:set_value(1); phase = "q"; t0 = t
  elseif phase == "q" and t - t0 > 0.3 then
    KEYS.q:clear_value(); phase = "quit"; t0 = t
  elseif phase == "quit" then
    if app_name() == "Finder" and t - t0 > 2 then
      print(string.format("t=%.1f the Finder is up (the save at quit: error %d, %d tries)", t,
        mem:read_i16(old_blk + 156), mem:read_i16(old_blk + 318))); phase = "finder"; t0 = t + 20
    elseif t - t0 > 120 then print("RESULT FAIL no Finder (" .. app_name() .. ")"); shot(); manager.machine:exit() end
  elseif phase == "finder" and t >= t0 then
    shot()
    local c = clicks[ci]
    if not c then
      phase = "back"; t0 = t; blk = nil; next_scan = t
      return
    end
    ci = ci + 1
    mouse_to(c[1], c[2]); phase = "click1"; t0 = t
  elseif phase == "click1" and t - t0 > 0.1 then btn:set_value(1); phase = "up1"; t0 = t
  elseif phase == "up1" and t - t0 > 0.08 then btn:clear_value(); phase = "click2"; t0 = t
  elseif phase == "click2" and t - t0 > 0.1 then btn:set_value(1); phase = "up2"; t0 = t
  elseif phase == "up2" and t - t0 > 0.08 then btn:clear_value(); phase = "finder"; t0 = t + 8
  elseif phase == "back" then                     -- (did the last click start the game again?)
    if not blk and t >= next_scan then
      blk = scan(); next_scan = t + 3
    end
    if blk and mem:read_u32(blk + 8) == 2 and mem:read_u32(blk + 40) >= 1 then
      local loaded, seed = mem:read_i16(blk + 152), mem:read_i32(blk + 292)
      print(string.format("t=%.1f the game is back: loaded %d (1 the player's world), seed %d; loading Blockworld World: error %d",
        t, loaded, seed, mem:read_i16(blk + 154)))
      screen:snapshot("finder_game.png")
      print(string.format("RESULT %s", (loaded == 1 and (not made or seed == made)) and "PASS" or "FAIL"))
      manager.machine:exit()
    elseif t - t0 > 90 then print("RESULT done (no game started)"); manager.machine:exit() end
  end
end)

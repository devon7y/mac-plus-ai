-- Blockworld M7: how long loading the world takes, in emulated time (Mac ticks), by phase.
-- Boots the game, prints how it got its world (1 the player's save, 2 the shipped starting world,
-- 0 made) and the time of each start-up step in emulated seconds, and the loading time's parts
-- (Mac ticks: reading the file, unpacking it, building the world), then saves and quits with
-- Esc, Q so that the restarted game loads the save, and prints the same again plus the save's
-- parts and the time from Q until the game was back.
-- LOOPS=<n> (default 1) restarts n times. Screenshots in $RUN_DIR/shots.
--   RAM=4M SLOW=1.5 emu/run_test.sh blockworld/build/blockworld.img blockworld/tests/loadtime.lua 400
local cpu = manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local screen = manager.machine.screens[":screen"]
local MAGIC, NOTMAGIC = 0x4257474D, 0xBDA8B8B2
local blk, last_blk, next_scan = nil, nil, 30
local loops = tonumber(os.getenv("LOOPS") or "1")
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
local KEYS = { q = find_char("q"), esc = find_named({"Esc"}) or find_char("`") }
local held = {}
local function hold(k, t, secs) KEYS[k]:set_value(1); held[k] = t + secs end
local function rd16(o) return mem:read_i16(blk + o) end
local function rdu(o) return mem:read_u32(blk + o) end
local phase, t0, n, tq, t1 = "boot", 0, 0, nil, nil
local s_last
local last_step, t_main = -1, nil
local function s(ticks) return ticks / 60.15 end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  for k, until_t in pairs(held) do
    if t >= until_t then KEYS[k]:clear_value(); held[k] = nil end
  end
  if not blk then
    if t >= next_scan then
      blk = scan(); next_scan = t + 0.5
      if blk then last_blk = blk; print(string.format("BWGM at %06x (t=%.1f) state %x", blk, t, rdu(8))) end
    end
    if t > 400 then print("RESULT FAIL no debug block"); manager.machine:exit() end
    return
  end
  local st = rdu(8)
  if st == 1 and not t1 then t1 = t end
  local step = mem:read_i16(blk + 290)
  if step ~= last_step then
    print(string.format("  t=%.2f step %d (state %x)", t, step, st))
    if step == 1 then t_main = t end
    last_step = step
  end
  if phase == "boot" and st == 2 and rdu(40) >= 1 then
    local loaded = rd16(152)
    print(string.format("t=%.1f playing: loaded %d (errors %d, default %d), seed %d, %s %.2f s: read %.2f, unpack %.2f, world %.2f; FreeMem %d",
      t, loaded, rd16(154), rd16(298), mem:read_i32(blk + 292), loaded >= 1 and "load" or "made", s(rdu(158)), s(rdu(274)), s(rdu(278)), s(rdu(282)), mem:read_i32(blk + 72)))
    print(string.format("  t=%.2f first frame (refinements %d)", t, rdu(40)))
    if tq then
      print(string.format("RESULT from Q to the game back: %.1f s (the restarted game began loading at %.1f s after Q)", t - tq, (t1 or t) - tq))
    end
    if t_main then print(string.format("RESULT start-up: main to first frame %.2f s (emulated)", t - t_main)) end
    if loaded >= 1 then
      print(string.format("RESULT load %.2f s (Mac ticks): read %.2f, unpack %.2f, world %.2f",
        s(rdu(158)), s(rdu(274)), s(rdu(278)), s(rdu(282))))
      screen:snapshot(string.format("loaded_%d.png", n))
      n = n + 1
    else
      screen:snapshot("made.png")
    end
    if loaded == 1 and n >= loops then manager.machine:exit() end
    hold("esc", t, 0.4)
    phase = "menu"; t0 = t
  elseif phase == "menu" and t - t0 > 1.5 then
    hold("q", t, 0.4)
    tq = t; t1 = nil; t_main = nil
    q_blk = blk
    phase = "saving"; t0 = t
  elseif phase == "saving" and rdu(8) == 2 and t - t0 > 0.5 and rdu(286) ~= 0 and rdu(286) ~= s_last then
    -- (the save's parts are in the block just before the game quits)
    print(string.format("RESULT save %.2f s (Mac ticks): pack %.2f, LZ %.2f, write %.2f; error %d (%d tries)",
      s(rdu(286)), s(rdu(300)), s(rdu(304)), s(rdu(308)), rd16(156), rd16(318)))
    s_last = rdu(286)
    mem:write_u32(blk, 0)                  -- (this game's block: not to be found again)
    blk = nil; next_scan = t + 0.5
    phase = "boot"
  elseif phase == "saving" and t - t0 > 60 then
    print("RESULT FAIL no save seen")
    mem:write_u32(blk, 0)
    blk = nil; next_scan = t + 0.5
    phase = "boot"
  end
end)

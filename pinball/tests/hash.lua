-- HASHTEST build: the app plays 3 minutes of the seeded AI game without drawing before it starts;
-- read the physics hash (must equal tests/gamesim --hash) and how long it took (emulated Ticks).
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local MAGIC = 0x4E50424C
local blk, next_scan = nil, 10
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if t < next_scan then return end
  next_scan = t + 2
  if not blk then
    for a = 0x100000 - 4, 0x1000, -2 do
      if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then blk = a; break end
    end
    if not blk then return end
  end
  local ticks = mem:read_i32(blk + 96)
  if ticks ~= 0 then
    local steps = 60 * 180 * 2
    print(string.format("hash %08x score %d: %d ticks for %d physics steps (+rules, AI) = %.1f us/step at MAME speed, %.0f%% of real time",
      mem:read_u32(blk + 68), mem:read_i32(blk + 24), ticks, steps, ticks / 60.15 * 1e6 / steps, 100 * ticks / (60 * 180)))
    local names = {"empty world, 1 ball", "empty world, 3 balls", "table, open area", "table, cradled",
                   "table, fast ball", "table, 3 balls", "table, open area high", "3 balls, flippers flap"}
    for k = 0, 7 do
      local t = mem:read_i32(blk + 108 + 4 * k)
      print(string.format("  bench %-22s %4d ticks / 2000 steps = %6.0f cycles per step (MAME 7.83 MHz)", names[k + 1], t,
        t / 60.15 / 2000 * 7833600))
    end
    manager.machine:exit()
  end
  if t > 600 then print("timeout"); manager.machine:exit() end
end)

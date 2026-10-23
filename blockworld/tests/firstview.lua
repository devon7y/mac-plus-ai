-- Blockworld: a screenshot of the first frame (shots/view.png), 2 s after the game is up; then quits.
-- Prints how the world was loaded and the start-up time. For choosing the starting world.
--   RAM=4M emu/run_test.sh <disk> blockworld/tests/firstview.lua 150
local cpu = manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local screen = manager.machine.screens[":screen"]
local MAGIC, NOTMAGIC = 0x4257474D, 0xBDA8B8B2
local blk, next_scan, t_up, t_main = nil, 30, nil, nil
local function scan()
  for a = 0x1000, mem:read_u32(0x108) - 8, 2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == NOTMAGIC then return a end
  end
end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not blk then
    if t >= next_scan then blk = scan(); next_scan = t + 0.5 end
    if t > 140 then print("RESULT FAIL no game"); manager.machine:exit() end
    return
  end
  if not t_main and mem:read_i16(blk + 290) >= 1 then t_main = t end
  if not t_up and mem:read_u32(blk + 8) == 2 and mem:read_u32(blk + 40) >= 1 then
    t_up = t
    mem:write_u16(blk + 214, 1)
    print(string.format("RESULT up at t=%.1f (%.1f s after main): loaded %d, seed %d, load %.2f s", t, t - t_main,
      mem:read_i16(blk + 152), mem:read_i32(blk + 292), mem:read_u32(blk + 158) / 60.15))
  end
  if t_up and t - t_up > 2 then screen:snapshot("view.png"); manager.machine:exit() end
end)

-- Orbit Duel: screenshots of the title screen and its demo (no input).
--   emu/run_test.sh duel/build/orbitduel.img duel/tests/title.lua 60
mem = manager.machine.devices[":maincpu"].spaces["program"]
screen = manager.machine.screens[":screen"]
local MAGIC = 0x4F44554C
local blk, n = nil, 0
local function scan()
  for a = 0x1000, 0xFFFF0, 2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then return a end
  end
end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if t > 20 + n * 5 and n < 4 then
    n = n + 1
    blk = blk or scan()
    if blk then
      print(string.format("t=%.1f screen=%d title_frames=%d front=%d tick=%d", t, mem:read_i32(blk + 12),
        mem:read_i32(blk + 228), mem:read_i32(blk + 232), mem:read_i32(blk + 28)))
    end
    screen:snapshot(string.format("title_%d.png", n))
    if n == 4 then manager.machine:exit() end
  end
end)

-- Rail Runner replay test: the REPLAY build plays a recorded autopilot run; this prints the
-- checkpoints the 68000 recorded (every 600 game ticks) for tests/replay_check.py.
dofile(os.getenv("EMU") .. "/scripts/lib.lua")
local MAGIC = 0x5252554E
local blk, next_scan = nil, 8
local want = tonumber(os.getenv("CHECKPOINTS") or "12")
local shots = 0
local function scan()
  local top = mem:read_u32(0x108)
  for a = top - 4, 0x1000, -2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then return a end
  end
end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not blk then
    if t >= next_scan then blk = scan(); next_scan = t + 4 end
    return
  end
  local ncp = mem:read_i16(blk + 446)
  local gt = mem:read_i32(blk + 102)
  if shots < 6 and gt > 300 + shots * 1200 then
    screen:snapshot(string.format("replay_%d.png", shots)); shots = shots + 1
  end
  local state = mem:read_i16(blk + 64)
  if ncp >= want or state ~= 0 or t > 900 then
    for i = 0, ncp - 1 do
      local c = blk + 190 + i * 16
      print(string.format("CP %d %d %d %d %d", (i + 1) * 600, mem:read_i32(c), mem:read_i32(c + 4), mem:read_i32(c + 8), mem:read_i32(c + 12)))
    end
    print(string.format("END t=%.1f game_ticks=%d state=%d frames=%d fps=%.2f", t, gt, state, mem:read_i32(blk + 16),
      mem:read_i32(blk + 16) / math.max(1, (mem:read_i32(blk + 20) - 0)) ))
    screen:snapshot("replay_end.png")
    manager.machine:exit()
  end
end)

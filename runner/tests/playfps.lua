-- Rail Runner real-play frame rate: run on a REPLAY build (the normal play loop with HUD, sound and
-- input, fed by the recorded autopilot); prints fps over 4 windows of 1200 game ticks, emulated Ticks.
--   SLOW=1.5 emu/run_test.sh runner/build/replay/replay.img runner/tests/playfps.lua 300
dofile(os.getenv("EMU") .. "/scripts/lib.lua")
local MAGIC = 0x5252554E
local blk, next_scan, m0, shots = nil, 8, nil, 0
local function scan()
  local top = mem:read_u32(0x108)
  for a = top - 4, 0x1000, -2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then return a end
  end
end
local function rd(o) return mem:read_i32(blk + o) end
local res = {}
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not blk then
    if t >= next_scan then blk = scan(); next_scan = t + 4 end
    if t > 120 then print("RESULT FAIL (no block)"); manager.machine:exit() end
    return
  end
  local gt = rd(102)
  if rd(12) == 2 and gt >= 300 then
    local k = math.floor((gt - 300) / 1200)
    if not res[k] then
      res[k] = {rd(16), rd(20)}
      if res[k - 1] then
        local f, tk = res[k][1] - res[k-1][1], res[k][2] - res[k-1][2]
        print(string.format("play window %d: %d frames in %d ticks -> %.2f fps (score %d)", k, f, tk, f * 60.15 / tk, rd(52)))
      end
      screen:snapshot(string.format("play_%d.png", k))
      if k >= 4 then manager.machine:exit() end
    end
  end
  if t > 400 then print("timeout"); manager.machine:exit() end
end)

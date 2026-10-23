-- Rail Runner M1 benchmark: find the 'RRUN' debug block, take screenshots while it runs,
-- then print the frame rate (emulated time, Mac Ticks) and per-stage timings.
--   emu/run_test.sh runner/build/bench384/bench384.img runner/tests/bench.lua 120
dofile(os.getenv("EMU") .. "/scripts/lib.lua")
local MAGIC = 0x5252554E
local blk
local shots = 0
local next_scan = 8

local function scan()
  local top = mem:read_u32(0x108)
  for a = top - 4, 0x1000, -2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then
      return a
    end
  end
end

local function rd(o) return mem:read_i32(blk + o) end
local function rd16(o) return mem:read_i16(blk + o) end

sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not blk then
    if t >= next_scan then
      blk = scan()
      next_scan = t + 4
      if blk then
        print(string.format("block at %06x (t=%.1fs) pf=%dx%d at (%d,%d) freemem=%d", blk, t,
          rd16(66), rd16(68), rd16(70), rd16(72), rd(90)))
      end
    end
    if t > 100 then print("RESULT FAIL (no block)"); manager.machine:exit() end
    return
  end
  local mode = rd(12)
  if mode == 2 and shots < 3 and rd(16) > 40 + shots * 120 then
    screen:snapshot(string.format("bench_%d.png", shots))
    print(string.format("shot %d at t=%.1f frames=%d z=%d nobj=%d ncoin=%d", shots, t, rd(16), rd(60), rd16(98), rd16(100)))
    shots = shots + 1
  end
  if mode == 4 then
    screen:snapshot("bench_end.png")
    local n = 30
    print(string.format("window: %d frames in %d ticks -> %.2f fps", rd(28), rd(32), rd(24) / 100))
    print(string.format("per frame: background %.2f ms, objects %.2f ms, blit %.2f ms; logic %.3f ms/tick",
      rd(36) / n * 16.625, rd(40) / n * 16.625, rd(44) / n * 16.625, rd(48) / (n * 4) * 16.625))
    print(string.format("objects by part (ms/frame): poles %.2f blocks %.2f faces %.2f coins %.2f player %.2f none %.2f",
      rd(126) / n * 16.625, rd(130) / n * 16.625, rd(134) / n * 16.625, rd(138) / n * 16.625, rd(142) / n * 16.625, rd(146) / n * 16.625))
    print(string.format("counters: fill_rect %d calls %d rows; vline %d calls %d rows; train rows %d; sprites %d (%d row-words); faces %d",
      rd(158), rd(162), rd(166), rd(170), rd(174), rd(178), rd(182), rd(186)))
    print(string.format("RESULT PASS fps=%.2f", rd(24) / 100))
    manager.machine:exit()
  end
  if t > 200 then print("RESULT FAIL (timeout, mode " .. mode .. ")"); manager.machine:exit() end
end)

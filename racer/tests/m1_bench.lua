-- Endurance M1 benchmark: find the 'EDBG' block, dump the check frame, print timings.
-- Run: emu/run_test.sh racer/build/m1_384.img racer/tests/m1_bench.lua 300
dofile(os.getenv("EMU") .. "/scripts/lib.lua")
local out = os.getenv("RUN_DIR") or "."
local base, next_scan, dumped = nil, 8, false
local function scan()
  for a = 0x1000, 0xFFFF0, 2 do
    if mem:read_u32(a) == 0x45444247 and mem:read_u32(a + 4) == 0xBABBBDB8 then return a end
  end
end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not base then
    if t >= next_scan then
      base = scan(); next_scan = t + 2
      if base then print(string.format("EDBG at %06x (t=%.1fs)", base, t)) end
    end
    if t > 120 then print("TIMEOUT no block"); manager.machine:exit() end
    return
  end
  local st = mem:read_u32(base + 8)
  if st == 0xDEAD then print("OUT OF MEMORY"); manager.machine:exit() end
  if st >= 1 and not dumped then
    local w, h, chk = mem:read_u16(base + 20), mem:read_u16(base + 22), mem:read_u32(base + 16)
    local f = io.open(out .. "/check.bin", "wb")
    local rbv = mem:read_u32(base + 60)
    for y = 0, h - 1 do for x = 8, 8 + w // 8 - 1 do f:write(string.char(mem:read_u8(chk + y * rbv + x))) end end
    f:close(); dumped = true
    print(string.format("check frame %dx%d dumped (t=%.1fs) scr=%06x buf=%06x bounds=%d,%d,%d,%d", w, h, t,
      mem:read_u32(base + 44), mem:read_u32(base + 56), mem:read_i16(base + 48), mem:read_i16(base + 50),
      mem:read_i16(base + 52), mem:read_i16(base + 54)))
  end
  if st == 2 then
    local n = mem:read_u32(base + 40)
    for i, name in ipairs({"A full", "B geom", "C draw", "D copy"}) do
      local ticks = mem:read_u32(base + 20 + 4 * i)
      print(string.format("RESULT %s: %d frames in %d ticks = %.2f ms/frame = %.1f fps", name, n, ticks,
        ticks / 60.15 / n * 1000, n / (ticks / 60.15)))
    end
    screen:snapshot("m1.png")
    manager.machine:exit()
  end
  if t > 280 then print("TIMEOUT status " .. st); manager.machine:exit() end
end)

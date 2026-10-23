-- Blockworld renderer benchmark: find the 'BWBN' block, dump each window's check frames
-- (RUN_DIR/check_<cfg>_<m|s>.bin: the column-pair buffer, + a screenshot), acknowledge them,
-- then print the timings.
-- Run: RAM=4M SLOW=1.5 emu/run_test.sh blockworld/build/bwbench.img blockworld/tests/m1_bench.lua 1500
-- Then: python3 blockworld/tests/verify_m1.py RUN_DIR
dofile(os.getenv("EMU") .. "/scripts/lib.lua")
local out = os.getenv("RUN_DIR") or "."
local MAGIC, NOTMAGIC = 0x4257424E, 0xBDA8BDB1
local base, next_scan, last_state, dumped = nil, 60, -1, {}
local function scan()
  for a = 0x1000, mem:read_u32(0x108) - 8, 2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == NOTMAGIC then return a end
  end
end
local CFG = {"320x240/12", "288x216/12", "256x192/12", "320x240/10", "288x216/10", "256x192/10"}
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not base then
    if t >= next_scan then
      base = scan(); next_scan = t + 5
      if base then print(string.format("BWBN at %06x (t=%.1fs)", base, t)) end
    end
    if t > 400 then print("TIMEOUT no block"); manager.machine:exit() end
    return
  end
  local st = mem:read_u32(base + 8)
  if st ~= last_state then
    print(string.format("t=%.1f state %d cfg %d", t, st, mem:read_u32(base + 12)))
    if last_state == 5 then
      print(string.format("RESULT selftest: %d frames, %d differ (68000 vs C: runs and buffers), %.1f s",
        mem:read_u32(base + 440), mem:read_u32(base + 444), mem:read_u32(base + 452) / 60.15))
    end
    last_state = st
  end
  if st == 0xDEAD then print("OUT OF MEMORY"); manager.machine:exit() end
  if st == 3 then
    local cfg, which = mem:read_u32(base + 12), mem:read_u32(base + 448)
    local key = cfg * 2 + which
    if not dumped[key] then
      local buf, n = mem:read_u32(base + 16), mem:read_u32(base + 460)
      local tag = which == 0 and "m" or "s"
      local f = io.open(string.format("%s/check_%d_%s.bin", out, cfg, tag), "wb")
      for i = 0, n - 1 do f:write(string.char(mem:read_u8(buf + i))) end
      f:close()
      screen:snapshot(string.format("check_%d_%s.png", cfg, tag))
      dumped[key] = true
      print(string.format("check frame %d %s dumped: %s", cfg, tag, CFG[cfg + 1]))
      mem:write_u32(base + 28, 1)
    end
  end
  if st == 4 then
    local gen = mem:read_u32(base + 32)
    print(string.format("RESULT world generation %.1f s (segments %.1f s); FreeMem %d", gen / 60.15,
      mem:read_u32(base + 464) / 60.15, mem:read_u32(base + 36)))
    local f = io.open(out .. "/results.txt", "w")
    for c = 0, mem:read_u32(base + 40) - 1 do
      local r = base + 52 + c * 64
      local frames = mem:read_u32(r + 16)
      local function ms(off) return mem:read_u32(r + off) / 60.15 / frames * 1000 end
      local line = string.format("%-11s moving %.1f ms = %.2f fps (cast %.1f, raster8 %.1f, present %.1f) | refine %.1f ms" ..
        " (edges %.1f, raster %.1f) | still from scratch %.1f ms (cast %.1f)", CFG[c + 1], ms(0), 1000 / ms(0), ms(4), ms(8),
        ms(12), ms(20), ms(28), ms(32), ms(36), ms(40))
      print("RESULT " .. line)
      f:write(line .. "\n")
    end
    f:close()
    manager.machine:exit()
  end
end)

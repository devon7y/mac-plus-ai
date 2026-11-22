-- PC sampler for Neural Pinball: once the debug block is found, waits PROF_DELAY s (default 8)
-- and records the 68000 PC every frame for PROF_SECS s (default 30) into $RUN_DIR/pc_samples.txt
-- ("pc anchor" per line); then tests/profile.py maps them to functions.
local cpu = manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local MAGIC = 0x4E50424C
local delay, secs = tonumber(os.getenv("PROF_DELAY") or "8"), tonumber(os.getenv("PROF_SECS") or "30")
local blk, t0, next_scan, f = nil, nil, 10, nil
local function scan()
  for a = 0x100000 - 4, 0x1000, -2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then return a end
  end
end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not blk then
    if t >= next_scan then blk = scan(); next_scan = t + 3; if blk then t0 = t; print(string.format("block %06x at %.1f", blk, t)) end end
    return
  end
  if os.getenv("PROF_MULTI") and t - t0 >= delay - 2 and not multi_done then
    mem:write_u32(blk + 100, 2)            -- start a multiball in the AI's game
    multi_done = true
  end
  if t - t0 < delay then return end
  if not f then f = io.open(os.getenv("RUN_DIR") .. "/pc_samples.txt", "w") end
  if t - t0 < delay + secs then
    f:write(string.format("%d %d\n", cpu.state["PC"].value, mem:read_u32(blk + 104)))
  else
    f:close()
    print(string.format("fps %.1f ticks/s %.1f late %d", mem:read_i32(blk + 36) / 100, mem:read_i32(blk + 40) / 100, mem:read_i32(blk + 72)))
    manager.machine:exit()
  end
end)

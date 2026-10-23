-- PC-sampling profiler for BWBench: after configuration PROF_CFG's check frame (state 3 -> 2),
-- sample the 68000 PC every emulated frame for PROF_SECONDS into RUN_DIR/pcs.txt
-- (first line: "main <runtime address>"). Symbolize: python3 blockworld/tests/pcprof.py RUN_DIR/pcs.txt
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local cpu = manager.machine.devices[":maincpu"]
local secs = tonumber(os.getenv("PROF_SECONDS") or "40")
local want = tonumber(os.getenv("PROF_CFG") or "0")
local out = io.open((os.getenv("RUN_DIR") or ".") .. "/pcs.txt", "w")
local base, t0, next_scan, seen3 = nil, nil, 60, false
local function scan()
  for a = 0x1000, mem:read_u32(0x108) - 8, 2 do
    if mem:read_u32(a) == 0x4257424E and mem:read_u32(a + 4) == 0xBDA8BDB1 then return a end
  end
end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not base then
    if t >= next_scan then base = scan(); next_scan = t + 5
      if base then out:write(string.format("main %d\n", mem:read_u32(base + 436))) end end
    return
  end
  local st, cfg = mem:read_u32(base + 8), mem:read_u32(base + 12)
  if st == 3 then mem:write_u32(base + 28, 1); if cfg == want then seen3 = true end end
  if seen3 and st == 2 and not t0 then t0 = t; print(string.format("sampling from t=%.1f", t)) end
  if t0 then
    if t - t0 < secs then out:write(string.format("%d\n", cpu.state["PC"].value))
    else out:close(); print("samples written"); manager.machine:exit() end
  end
end)

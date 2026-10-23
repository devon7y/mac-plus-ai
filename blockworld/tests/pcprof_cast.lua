-- Profile the 68000 caster alone: set the block's profile flag, wait for state 6 (the caster
-- loops forever), sample the PC every emulated frame for PROF_SECONDS into RUN_DIR/pcs.txt.
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local cpu = manager.machine.devices[":maincpu"]
local secs = tonumber(os.getenv("PROF_SECONDS") or "30")
local out = io.open((os.getenv("RUN_DIR") or ".") .. "/pcs.txt", "w")
local base, t0, next_scan = nil, nil, 60
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not base then
    if t >= next_scan then
      next_scan = t + 5
      for a = 0x1000, mem:read_u32(0x108) - 8, 2 do
        if mem:read_u32(a) == 0x4257424E and mem:read_u32(a + 4) == 0xBDA8BDB1 then base = a; break end
      end
      if base then mem:write_u32(base + 476, 1); out:write(string.format("main %d\n", mem:read_u32(base + 436))) end
    end
    return
  end
  if not t0 then
    if mem:read_u32(base + 8) == 6 then t0 = t + 2; print(string.format("profiling from t=%.1f", t0)) end
    return
  end
  if t < t0 then return end
  if t - t0 < secs then out:write(string.format("%d\n", cpu.state["PC"].value))
  else out:close(); print("samples written"); manager.machine:exit() end
end)

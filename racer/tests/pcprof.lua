-- PC-sampling profiler: after the game starts (drive: start + hold the button), sample the
-- 68000 PC once per emulated frame for PROF_SECONDS and write "pc" lines to $RUN_DIR/pcs.txt.
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local cpu = manager.machine.devices[":maincpu"]
local mport = manager.machine.ioport.ports[":MOUSE0"]
local btn = mport and mport.fields["Mouse Button"]
local base, t0, f
local secs = tonumber(os.getenv("PROF_SECONDS") or "30")
local out = io.open((os.getenv("RUN_DIR") or ".") .. "/pcs.txt", "w")
local function scan()
  local zone = mem:read_u32(0x2AA)
  for a = zone - zone % 2, zone + 0x180000, 2 do
    if mem:read_u32(a) == 0x454E4455 and mem:read_u32(a + 8) == 0xBAB1BBAA then return a end
  end
end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not base then
    if t > 22 then base = scan(); t0 = t
      if base then mem:write_u32(base + 48, 4242); mem:write_u16(base + 52, 1)
        out:write(string.format("main %d\n", mem:read_u32(base + 132))) end
    end
    return
  end
  local e = t - t0
  if e > 1 and btn then btn:set_value(1) end
  if e > 6 and e < 6 + secs then out:write(string.format("%d\n", cpu.state["PC"].value)) end
  if e >= 6 + secs then out:close(); print("samples written"); manager.machine:exit() end
end)

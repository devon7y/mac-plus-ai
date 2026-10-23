-- Print the 68000 PC and registers a few times (debugging hangs).
local cpu = manager.machine.devices[":maincpu"]
local n = 0
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if t > 25 + n * 0.37 and n < 8 then
    n = n + 1
    local s = cpu.state
    print(string.format("t=%.2f PC=%06x SR=%04x A5=%06x D0=%08x A0=%06x", t, s["PC"].value, s["SR"].value,
      s["A5"].value, s["D0"].value, s["A0"].value))
  end
  if n >= 8 then manager.machine:exit() end
end)

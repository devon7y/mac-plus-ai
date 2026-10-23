-- Screenshots at the emulated times in $SNAP_TIMES (comma-separated), then exit.
local times = {}
for t in string.gmatch(os.getenv("SNAP_TIMES") or "30,60,90", "[^,]+") do times[#times + 1] = tonumber(t) end
local i = 1
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if i <= #times and t >= times[i] then
    manager.machine.video:snapshot()
    print(string.format("SHOT %.0f", t))
    i = i + 1
    if i > #times then manager.machine:exit() end
  end
end)

-- Screenshots at SNAP_TIMES (comma-separated emulated seconds), then exit.
local times = {}
for v in string.gmatch(os.getenv("SNAP_TIMES") or "60", "[^,]+") do times[#times + 1] = tonumber(v) end
local i = 1
local scr = manager.machine.screens[":screen"]
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if i <= #times and t >= times[i] then scr:snapshot(string.format("snap_%03d.png", times[i])); print("snap " .. times[i]); i = i + 1 end
  if i > #times then manager.machine:exit() end
end)

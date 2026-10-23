-- Boot test: save screenshots at fixed emulated times, then exit.
local shots = { 5, 15, 25, 35, 45 }
local quit_at = 50
local idx = 1
local screen = manager.machine.screens[":screen"]
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if idx <= #shots and t >= shots[idx] then
    screen:snapshot(string.format("boot_%02ds.png", shots[idx]))
    idx = idx + 1
  end
  if t >= quit_at then manager.machine:exit() end
end)

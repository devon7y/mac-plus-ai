-- First boot of a freshly installed System 7.0.1 disk (macsefd): wait for the Finder, give it
-- 20 s to build its desktop file, then choose Special > Shut Down so the volume is left clean.
dofile(os.getenv("EMU") .. "/../toolchain/se7/se_lib.lua")
local fat, t0
sub2 = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not fat and app_name() == "Finder" then fat = t; print(string.format("Finder running at t=%.1f", t)) end
  if not fat then
    if t > 300 then print("TIMEOUT waiting for the Finder"); screen:snapshot("timeout.png"); manager.machine:exit() end
    return
  end
  if t < fat + 20 then return end
  if not t0 then t0 = t; screen:snapshot("finder.png"); mouse_to(237, 9); btn:set_value(1) end   -- Special menu
  if t > t0 + 0.5 and t < t0 + 1.0 then mouse_to(250, 139) end                                  -- Shut Down
  if t > t0 + 1.0 and t < t0 + 1.5 then btn:clear_value() end
  if t > t0 + 25 then screen:snapshot("shutdown.png"); print(string.format("t=%.0f shut down", t)); manager.machine:exit() end
end)

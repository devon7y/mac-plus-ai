local screen = manager.machine.screens[":screen"]
local shots = { 70, 100, 130 }
local i = 1
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if i <= #shots and t >= shots[i] then screen:snapshot(string.format("se720_%02d.png", shots[i])); i = i + 1 end
  if t >= 131 then
    for tag, img in pairs(manager.machine.images) do if tag:find("fdc") then print(tag, img.exists and img.filename or "(empty)") end end
    manager.machine:exit()
  end
end)

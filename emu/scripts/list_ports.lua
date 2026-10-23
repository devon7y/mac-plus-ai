sub = emu.add_machine_frame_notifier(function()
  for tag, port in pairs(manager.machine.ioport.ports) do
    for name, f in pairs(port.fields) do
      if not tag:find("kbd") or name:find("Shift") or name:find("Command") or name:find("Return") then
        print(string.format("%-40s %-28s type=%-14s mask=%08x analog=%s", tag, name, tostring(f.type), f.mask, tostring(f.is_analog)))
      end
    end
  end
  local n = 0; for tag, port in pairs(manager.machine.ioport.ports) do if tag:find("kbd") then for _ in pairs(port.fields) do n = n + 1 end end end
  print("keyboard fields total:", n)
  for tag, _ in pairs(manager.machine.devices) do if tag:find("maincpu") or tag:find("ram") then print("device:", tag) end end
  manager.machine:exit()
end)

-- Print every emulated Mac Plus keyboard key: port tag, field name, mask.
local rows = {}
for tag, port in pairs(manager.machine.ioport.ports) do
  if tag:find("kbd") then
    for name, f in pairs(port.fields) do rows[#rows + 1] = string.format("%-18s %-22s 0x%08x", tag, name, f.mask) end
  end
end
table.sort(rows); for _, r in ipairs(rows) do print(r) end
manager.machine:exit()

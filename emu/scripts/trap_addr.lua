local mem = manager.machine.devices[":maincpu"].spaces["program"]
sub = emu.add_machine_frame_notifier(function()
  if manager.machine.time:as_double() >= 30 then
    for _, tr in ipairs({ {"InitGraf",0x6E}, {"InitWindows",0x112}, {"InitMenus",0x130}, {"InitCursor",0x50}, {"ShowCursor",0x53}, {"HideCursor",0x52}, {"DrawMenuBar",0x137}, {"OpenPort",0x6F}, {"InitPort",0x6D} }) do
      print(string.format("%-12s A8%02X -> %06x", tr[1], tr[2] & 0xFF, mem:read_u32(0xC00 + tr[2]*4)))
    end
    manager.machine:exit()
  end
end)

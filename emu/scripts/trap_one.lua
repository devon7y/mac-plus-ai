local mem = manager.machine.devices[":maincpu"].spaces["program"]
sub = emu.add_machine_frame_notifier(function()
  if manager.machine.time:as_double() >= 30 then
    print(string.format("ScrnBitMap A833 -> %06x", mem:read_u32(0xC00 + 0x33*4)))
    manager.machine:exit()
  end
end)

-- Print sound-related low memory and VIA state at t=30 s.
local mem = manager.machine.devices[":maincpu"].spaces["program"]
sub = emu.add_machine_frame_notifier(function()
  if manager.machine.time:as_double() > 30 then
    local sb = mem:read_u32(0x266)
    print(string.format("SoundBase=%06x MemTop=%06x ScrnBase=%06x SdVolume=%02x", sb, mem:read_u32(0x108), mem:read_u32(0x824), mem:read_u8(0x260)))
    print(string.format("VIA ORB=%02x ORA=%02x DDRB=%02x DDRA=%02x", mem:read_u8(0xEFE1FE), mem:read_u8(0xEFFFFE), mem:read_u8(0xEFE5FE), mem:read_u8(0xEFE7FE)))
    local s = {}
    for i = 0, 15 do s[#s + 1] = string.format("%04x", mem:read_u16(sb + i * 2)) end
    print("buffer: " .. table.concat(s, " "))
    manager.machine:exit()
  end
end)

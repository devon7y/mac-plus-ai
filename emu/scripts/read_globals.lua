local mem = manager.machine.devices[":maincpu"].spaces["program"]
local marks = { 3, 10, 30 }
local i = 1
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if i <= #marks and t >= marks[i] then
    print(string.format("t=%2ds ScrnBase=%06x ScreenRow=%d RowBits=%d ColLines=%d ScreenBytes=%d CrsrPin=(%d,%d,%d,%d) MemTop=%06x",
      marks[i], mem:read_u32(0x824), mem:read_u16(0x106), mem:read_u16(0xC20), mem:read_u16(0xC22), mem:read_u32(0xC24),
      mem:read_i16(0x834), mem:read_i16(0x836), mem:read_i16(0x838), mem:read_i16(0x83A), mem:read_u32(0x108)))
    i = i + 1
    if i > #marks then manager.machine:exit() end
  end
end)

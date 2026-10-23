dofile(os.getenv("EMU") .. "/scripts/lib.lua")
local last = ""
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  local s = string.format("ScrnBase=%06x ScrnBitMap=%06x MemTop=%06x KeyMap7=%02x", mem:read_u32(0x824), mem:read_u32(0xC00 + 0x33*4), mem:read_u32(0x108), mem:read_u8(0x17B))
  if s ~= last then print(string.format("t=%6.2f %s", t, s)); last = s end
  if t > 75 then manager.machine:exit() end
end)

-- Shared helpers for scripted MAME sessions.
mem = manager.machine.devices[":maincpu"].spaces["program"]
screen = manager.machine.screens[":screen"]
btn = manager.machine.ioport.ports[":MOUSE0"].fields["Mouse Button"]
function mouse_to(h, v)   -- absolute cursor move (global coordinates)
  mem:write_u16(0x828, v); mem:write_u16(0x82A, h)
  mem:write_u16(0x82C, v); mem:write_u16(0x82E, h)
  mem:write_u8(0x8CE, mem:read_u8(0x8CF))
end
function globals(tag)
  print(string.format("[%s] ScrnBase=%06x ScrnBitMap=%06x CrsrPin=(%d,%d,%d,%d) Mouse=(%d,%d)", tag,
    mem:read_u32(0x824), mem:read_u32(0xC00 + 0x33*4), mem:read_i16(0x834), mem:read_i16(0x836),
    mem:read_i16(0x838), mem:read_i16(0x83A), mem:read_i16(0x832), mem:read_i16(0x830)))
end
function run_steps(steps)
  local i = 1
  sub = emu.add_machine_frame_notifier(function()
    local t = manager.machine.time:as_double()
    while i <= #steps and t >= steps[i][1] do steps[i][2](); i = i + 1 end
  end)
end

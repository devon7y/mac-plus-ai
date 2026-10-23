dofile(os.getenv("EMU") .. "/scripts/lib.lua")
local function trace(tag)
  print(string.format("[%5s] calls=%d msg=%d item=%d numItems=%d part=%d valBefore=%d MBState=%02x", tag,
    mem:read_u16(0xA78), mem:read_i16(0xA7A), mem:read_i16(0xA7C), mem:read_i16(0xA7E),
    mem:read_i16(0xA80), mem:read_i16(0xA82), mem:read_u8(0x172)))
end
local steps = {
  { 28.0, function() mouse_to(12, 8) end }, { 28.3, function() btn:set_value(1) end },
  { 28.8, function() mouse_to(60, 107) end }, { 29.3, function() btn:clear_value() end },
  { 45.0, function() mouse_to(98, 258) end }, { 45.2, function() btn:set_value(1) end }, { 45.4, function() btn:clear_value() end },
  { 55.0, function() trace("open"); for a = 0xA78, 0xA84, 2 do mem:write_u16(a, 0) end end },
  { 56.0, function() mouse_to(325, 130) end },
  { 56.2, function() btn:set_value(1); trace("down") end },
  { 56.4, function() btn:clear_value(); trace("up") end },
}
for _, dt in ipairs({ 0.6, 1.0, 2.0, 4.0, 8.0 }) do
  table.insert(steps, { 56.4 + dt, function() trace(string.format("+%.1f", dt)); screen:snapshot(string.format("trace_%04.1f.png", dt)) end })
end
table.insert(steps, { 65, function() manager.machine:exit() end })
run_steps(steps)

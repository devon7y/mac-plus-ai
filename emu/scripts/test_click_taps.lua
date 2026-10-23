dofile(os.getenv("EMU") .. "/scripts/lib.lua")
local cpu = manager.machine.devices[":maincpu"]
local function reg(n) local s = cpu.state[n]; return s and s.value or -1 end
local function dump(tag)
  local sp = reg("SP")
  local words = {}
  for i = 0, 23 do words[#words + 1] = string.format("%04x", mem:read_u16(sp + i * 2)) end
  print(string.format("%s PC=%06x SR=%04x SP=%06x A0=%06x A1=%06x A2=%06x A3=%06x A4=%06x A5=%06x A6=%06x D0=%08x",
    tag, reg("PC"), reg("SR"), sp, reg("A0"), reg("A1"), reg("A2"), reg("A3"), reg("A4"), reg("A5"), reg("A6"), reg("D0")))
  print("  stack: " .. table.concat(words, " "))
end
local hits = 0
local steps = {
  { 28.0, function() mouse_to(12, 8) end }, { 28.3, function() btn:set_value(1) end },
  { 28.8, function() mouse_to(60, 107) end }, { 29.3, function() btn:clear_value() end },
  { 45.0, function() mouse_to(98, 258) end }, { 45.2, function() btn:set_value(1) end }, { 45.4, function() btn:clear_value() end },
  { 55.0, function()
      local function tap(name) return function(offset, data, mask) if hits < 6 then hits = hits + 1; dump(name .. string.format("@%x", offset)) end; return data end end
      tap_ill = mem:install_read_tap(0x10, 0x13, "illegal", tap("VEC4-illegal"))
      tap_adr = mem:install_read_tap(0x0C, 0x0F, "addrerr", tap("VEC3-addrerr"))
      tap_bus = mem:install_read_tap(0x08, 0x0B, "buserr", tap("VEC2-buserr"))
      tap_se  = mem:install_read_tap(0xC00 + 0x1C9*4, 0xC00 + 0x1C9*4 + 3, "syserr", tap("SysError-dispatch"))
      local mh = 0
      tap_mouse = mem:install_read_tap(0x830, 0x833, "mouse", function(o, d, m) mh = mh + 1; if mh == 1 or mh == 1000 then print("mouse global read #" .. mh) end; return d end)
      print("taps installed")
    end },
  { 56.0, function() mouse_to(325, 130) end },
  { 56.2, function() btn:set_value(1) end },
  { 56.4, function() btn:clear_value() end },
  { 62.0, function() screen:snapshot("taps_after.png"); manager.machine:exit() end },
}
run_steps(steps)

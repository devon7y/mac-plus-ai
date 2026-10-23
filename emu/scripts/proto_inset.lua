-- Prototype: emulate what the INIT will do, by poking memory during boot.
local L, R = 32, 32                      -- left margin (multiple of 16), right margin
local W = 512 - L - R
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local screen = manager.machine.screens[":screen"]
local btn = manager.machine.ioport.ports[":MOUSE0"].fields["Mouse Button"]
local function mouse_to(h, v)            -- absolute move via Mac low-memory globals
  mem:write_u16(0x828, v); mem:write_u16(0x82A, h)   -- MTemp
  mem:write_u16(0x82C, v); mem:write_u16(0x82E, h)   -- RawMouse
  mem:write_u8(0x8CE, mem:read_u8(0x8CF))            -- CrsrNew = CrsrCouple
end
local steps = {
  { 12, function()
      local base = mem:read_u32(0x824)
      print(string.format("before: ScrnBase=%06x ROM imm=%d", base, mem:read_u16(0x401e6e)))
      mem:write_u32(0x824, base + L // 8)               -- shift origin right
      mem:write_direct_u16(0x401e6e, W)                 -- ScrnBitMap reports width W
      mem:write_u16(0x83A, W)                           -- CrsrPin.right
      for y = 0, 341 do                                 -- paint margins black
        for b = 0, L // 8 - 1 do mem:write_u8(base + y*64 + b, 0xFF) end
        for b = (L + W) // 8, 63 do mem:write_u8(base + y*64 + b, 0xFF) end
      end
      print(string.format("after:  ScrnBase=%06x ROM imm=%d CrsrPin.right=%d", mem:read_u32(0x824), mem:read_u16(0x401e6e), mem:read_u16(0x83A)))
    end },
  { 35, function() screen:snapshot("proto_desktop.png") end },
  { 36, function() mouse_to(600, 200) end },                       -- try to push cursor far right
  { 37, function() print(string.format("mouse after push right: h=%d v=%d", mem:read_i16(0x832), mem:read_i16(0x830))); mouse_to(12, 8) end },
  { 38, function() btn:set_value(1) end },                         -- press on Apple menu
  { 39, function() screen:snapshot("proto_applemenu.png") end },
  { 39.5, function() btn:clear_value() end },
  { 41, function() manager.machine:exit() end },
}
local i = 1
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  while i <= #steps and t >= steps[i][1] do steps[i][2](); i = i + 1 end
end)

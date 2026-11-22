-- Broadside in MAME: the About box and the Statistics window (mailbox commands 7 and 8), one
-- screenshot each, then a click to close.  emu/run_test.sh <disk> broadside/tests/windows.lua 120
mem = manager.machine.devices[":maincpu"].spaces["program"]
screen = manager.machine.screens[":screen"]
local ports = manager.machine.ioport.ports
btn = (ports[":macadb:MOUSE0"] and ports[":macadb:MOUSE0"].fields["Mouse Button 0"]) or ports[":MOUSE0"].fields["Mouse Button"]
local function ram_top()
  local n, u = manager.options.entries["ramsize"]:value():match("(%d+)(%a?)")
  return tonumber(n) * ((u == "M" or u == "m") and 1048576 or (u == "K" or u == "k") and 1024 or 1)
end
local MAGIC, blk = 0x42524453, nil
local function scan()
  for a = ram_top() - 8, 0x1000, -2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then return a end
  end
end
local function f(o) return mem:read_u32(blk + o) end
local steps, i, nextT = {}, 1, tonumber(os.getenv("CHECK_AT") or "25")
local function cmd(c) mem:write_u32(blk + 20, c) end
local seq = {
  function() blk = scan(); return blk ~= nil and f(12) >= 1 end,
  function() cmd(7); return true end,
  function() return f(40) == 7 end,
  function() screen:snapshot("about.png"); btn:set_value(1); return true end,
  function() btn:clear_value(); return f(20) == 0 end,
  function() cmd(8); return true end,
  function() return f(40) == 8 end,
  function() screen:snapshot("stats.png"); btn:set_value(1); return true end,
  function() btn:clear_value(); return f(20) == 0 end,
  function() print("RESULT PASS"); manager.machine:exit(); return true end,
}
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if t < nextT then return end
  nextT = t + 1
  if seq[i]() then i = i + 1 end
end)

-- Helpers for scripted MAME runs that work on both macplus and macsefd (SE FDHD, ADB mouse).
-- (emu/scripts/lib.lua assumes the Plus's :MOUSE0 port and fails on the SE.)
mem = manager.machine.devices[":maincpu"].spaces["program"]
screen = manager.machine.screens[":screen"]
local p = manager.machine.ioport.ports
btn = (p[":macadb:MOUSE0"] and p[":macadb:MOUSE0"].fields["Mouse Button 0"])
   or (p[":MOUSE0"] and p[":MOUSE0"].fields["Mouse Button"])
function mouse_to(h, v)   -- absolute cursor move (global coordinates), via the low-memory mouse globals
  mem:write_u16(0x828, v); mem:write_u16(0x82A, h)
  mem:write_u16(0x82C, v); mem:write_u16(0x82E, h)
  mem:write_u8(0x8CE, mem:read_u8(0x8CF))
end
function ram_top()        -- physical RAM size (MemTop at $108 is not RAM top under System 7)
  local s = manager.options.entries["ramsize"]:value()
  local n, u = s:match("(%d+)(%a?)")
  n = tonumber(n)
  if u == "M" or u == "m" then n = n * 1024 * 1024 elseif u == "K" or u == "k" then n = n * 1024 end
  return n
end
function app_name()
  local n, s = mem:read_u8(0x910), ""
  for i = 1, math.min(n, 31) do s = s .. string.char(mem:read_u8(0x910 + i)) end
  return s
end
function floppy(i)         -- image device of floppy drive i (0 or 1)
  for tag, img in pairs(manager.machine.images) do
    if tag:match("^:fdc:" .. i .. ":") then return img end
  end
end
-- Timed steps: steps = {{t, fn}, ...} (emulated seconds). Steps may return a number to be
-- re-run that many seconds later (polling).
function run_steps(steps)
  local i, again = 1, nil
  sub = emu.add_machine_frame_notifier(function()
    local t = manager.machine.time:as_double()
    if again and t < again then return end
    while i <= #steps and t >= steps[i][1] do
      local r = steps[i][2](t)
      if type(r) == "number" then again = t + r; return end
      again = nil; i = i + 1
    end
  end)
end

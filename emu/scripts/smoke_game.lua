-- One game's smoke run for emu/smoke_all.sh (works on macplus and macsefd).
-- Environment (set by smoke_all.sh):
--   SMOKE_TIMES    "20,45,90"   emulated seconds at which to save shots/tNNN.N.png
--   SMOKE_ACTIONS  "60,key,Return;62,hold,Right,3;70,click,256,171;75,type,hello"
--     key  NAME [secs]   press and release (default 0.15 s)    hold NAME secs   same, longer
--     click H V          mouse to (H, V) global coordinates, click   type TEXT   one key per character
--     crash              jump to an ILLEGAL instruction (only to test smoke_all.sh's detectors)
--   Key names: Return Tab Space Esc Up Down Left Right Command Option Shift Backspace, or a
--   single letter / digit (an uppercase letter is typed with Shift).
-- Also reinserts the floppy after a restart ejects it, like emu/play.sh (scripts/autoreinsert.lua).
dofile(os.getenv("EMU") .. "/scripts/autoreinsert.lua")      -- uses the global `sub`

local cpu = manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local screen = manager.machine.screens[":screen"]
local ports = manager.machine.ioport.ports

local function find_field(names)
  for _, want in ipairs(names) do
    for _, port in pairs(ports) do
      for name, f in pairs(port.fields) do
        if name == want then return f end
      end
    end
  end
end
local function find_char(c)               -- fields are named like "x  X" and "1  !"
  local pat = "^" .. c:lower():gsub("%p", "%%%0") .. "  "
  for _, port in pairs(ports) do
    for name, f in pairs(port.fields) do
      if name:find(pat) then return f end
    end
  end
end
local named = {
  Return = {"Return"}, Tab = {"Tab"}, Space = {"Space"}, Esc = {"Esc"},
  Up = {"Cursor Up", "Up Arrow"}, Down = {"Cursor Down", "Down Arrow"},
  Left = {"Cursor Left", "Left Arrow"}, Right = {"Cursor Right", "Right Arrow"},
  Command = {"Command", "Command / Open Apple"}, Option = {"Option"}, Shift = {"Shift"},
  Backspace = {"Backspace", "Delete"},
}
local function key_fields(name)            -- list of fields to press for a key name
  if named[name] then local f = find_field(named[name]); return f and {f} or nil end
  if #name == 1 then
    local f = find_char(name)
    if not f then return nil end
    if name:match("%u") then return {find_field(named.Shift), f} end
    return {f}
  end
end
local button = (ports[":macadb:MOUSE0"] and ports[":macadb:MOUSE0"].fields["Mouse Button 0"])
            or (ports[":MOUSE0"] and ports[":MOUSE0"].fields["Mouse Button"])
local function mouse_to(h, v)
  mem:write_u16(0x828, v); mem:write_u16(0x82A, h)
  mem:write_u16(0x82C, v); mem:write_u16(0x82E, h)
  mem:write_u8(0x8CE, mem:read_u8(0x8CF))
end

-- Build a flat, time-sorted list of {t, fn} events.
local ev = {}
local function at(t, fn) ev[#ev + 1] = {t, fn} end
local function press(t, fields, secs, label)
  if not fields then print(string.format("SMOKE warning: unknown key %q", label)); return end
  at(t, function() for _, f in ipairs(fields) do f:set_value(1) end end)
  at(t + secs, function() for i = #fields, 1, -1 do fields[i]:clear_value() end end)
end
for item in (os.getenv("SMOKE_ACTIONS") or ""):gmatch("[^;]+") do
  local a = {}
  for x in item:gmatch("[^,]+") do a[#a + 1] = x end
  local t, kind = tonumber(a[1]), a[2]
  if kind == "key" or kind == "hold" then
    press(t, key_fields(a[3]), tonumber(a[4] or "0.15"), a[3])
  elseif kind == "click" then
    local h, v = tonumber(a[3]), tonumber(a[4])
    at(t, function() mouse_to(h, v) end)
    at(t + 0.05, function() button:set_value(1) end)
    at(t + 0.2, function() button:clear_value() end)
  elseif kind == "type" then
    local s = a[3] or ""
    for i = 1, #s do
      local c = s:sub(i, i)
      press(t + (i - 1) * 0.3, key_fields(c == " " and "Space" or c), 0.15, c)
    end
  elseif kind == "crash" then
    at(t, function()
      mem:write_u16(0x30000, 0x4AFC); mem:write_u16(0x30002, 0x4AFC)
      cpu.state["PC"].value = 0x30000
    end)
  else
    print(string.format("SMOKE warning: unknown action %q", item))
  end
end
local last = 0
for x in (os.getenv("SMOKE_TIMES") or "20,45,90"):gmatch("[%d%.]+") do
  local t = tonumber(x)
  at(t, function()
    local name = string.format("t%05.1f.png", t)            -- e.g. t045.0.png
    screen:snapshot(name)
    print(string.format("SMOKE shot t=%g %s", t, name))
  end)
  if t > last then last = t end
end
for _, e in ipairs(ev) do if e[1] > last then last = e[1] end end
at(last + 0.5, function() print("SMOKE done"); manager.machine:exit() end)
table.sort(ev, function(x, y) return x[1] < y[1] end)

local i = 1
smoke_sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  while i <= #ev and t >= ev[i][1] do ev[i][2](); i = i + 1 end
end)

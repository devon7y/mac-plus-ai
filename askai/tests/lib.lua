-- Shared helpers for the Ask AI MAME scripts (chat.lua, m2.lua): finds the app's debug block
-- (magic 'ASK!', see app/main.c struct Dbg), sets the null modem's speed, keys, menus, a step runner.
mem = manager.machine.devices[":maincpu"].spaces["program"]
local MAGIC = 0x41534B21
base = nil
local next_scan = 5
function now() return manager.machine.time:as_double() end
function i32(off) local v = mem:read_u32(base + off); if v >= 0x80000000 then v = v - 0x100000000 end; return v end
function str(off, n)
  local t = {}
  for k = 0, n - 1 do
    local c = mem:read_u8(base + off + k)
    if c == 0 then break end
    t[#t + 1] = string.char(c)
  end
  return table.concat(t)
end
function shot(name) manager.machine.video:snapshot(); print(string.format("SHOT %s t=%.1f", name, now())) end
function report(tag)
  print(string.format("[%s t=%.1f] state=%d rx=%d replies=%d errors=%d lastErr=%d rows=%d models=%d model=%s "
    .. "replyBytes=%d bps=%d serErr=%d free=%d note=%q last=%q", tag, now(), i32(12), i32(16), i32(24), i32(28),
    i32(32), i32(36), i32(40), str(132, 32), i32(44), i32(48), i32(52), i32(64), str(68, 64), str(164, 64)))
end
local function field(name)
  for _, port in pairs(manager.machine.ioport.ports) do
    for fname, f in pairs(port.fields) do
      if fname == name or fname:find("^" .. name .. "%s") then return f end
    end
  end
end
local releases = {}
function key(name, dur) local k = field(name); k:set_value(1); releases[#releases + 1] = {k = k, at = now() + (dur or 0.15)} end
function cmd_key(name)          -- Command + key
  local c = field("Command")
  c:set_value(1)
  releases[#releases + 1] = {k = c, at = now() + 0.6}
  local k = field(name)
  if not k then print("no key field " .. name) return end
  releases[#releases + 1] = {k = k, at = now() + 0.4, press = now() + 0.2}
end
function type_text(s) manager.machine.natkeyboard:post(s) end
local btn = manager.machine.ioport.ports[":MOUSE0"] and manager.machine.ioport.ports[":MOUSE0"].fields["Mouse Button"]
function mouse_to(h, v)          -- global (logical) coordinates
  mem:write_u16(0x828, v); mem:write_u16(0x82A, h)
  mem:write_u16(0x82C, v); mem:write_u16(0x82E, h)
  mem:write_u8(0x8CE, mem:read_u8(0x8CF))
end
function menu_steps(title_h, item_v)   -- press on a menu title, drag to an item, release
  return {
    {function() return true end, function() mouse_to(title_h, 8) end, 0.4},
    {function() return true end, function() btn:set_value(1) end, 0.6},
    {function() return true end, function() mouse_to(title_h + 10, item_v) end, 0.6},
    {function() return true end, function() btn:clear_value() end, 1.0},
  }
end

-- the null modem's speed: 21 = 7200, the app's MAME test speed (MAME clocks the Plus SCC 6.7% fast)
local baud = tonumber(os.getenv("BAUD") or "21")
for tag, port in pairs(manager.machine.ioport.ports) do
  if tag:find("null_modem") then
    for fname, f in pairs(port.fields) do
      if fname == "TX Baud" or fname == "RX Baud" then f.user_value = baud end
    end
  end
end

function run_steps(steps)
  local step, wait_until, reported_err = 1, 0, false
  sub = emu.add_machine_frame_notifier(function()
    local t = now()
    for k = #releases, 1, -1 do
      local r = releases[k]
      if r.press and t >= r.press then r.k:set_value(1); r.press = nil end
      if not r.press and t >= r.at then r.k:clear_value(); table.remove(releases, k) end
    end
    if t > tonumber(os.getenv("LIMIT") or "200") then
      if base then report("timeout") end
      shot("timeout")
      print("RESULT TIMEOUT"); manager.machine:exit() return
    end
    if not base then
      if t >= next_scan then
        next_scan = t + 2
        for a = 0x8000, 0x3FFFF0, 2 do
          if mem:read_u16(a) == 0x4153 and mem:read_u32(a) == MAGIC and mem:read_u32(a + 8) == 0xBEACB4DE then
            base = a; print(string.format("block at %06x t=%.1f", a, t)); break
          end
        end
      end
      return
    end
    if t < wait_until or #releases > 0 then return end
    local s = steps[step]
    if s and s[1]() then
      s[2]()
      wait_until = t + (s[3] or 0)
      step = step + 1
    end
  end)
end

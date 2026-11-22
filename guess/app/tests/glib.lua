-- Helpers for Guess My Doodle's MAME tests (adapted from paint/app/tests/dlib.lua): sequential
-- scripts (coroutines), the debug block ('GSDB'), mouse drags, keys. Plus (macplus) and SE (macsefd).
mem = manager.machine.devices[":maincpu"].spaces["program"]
screen = manager.machine.screens[":screen"]
for tag, port in pairs(manager.machine.ioport.ports) do
  for name, f in pairs(port.fields) do
    if name == "Mouse Button" or name == "Mouse Button 0" then btn = f end
  end
end
function ram_top()
  local v = manager.options.entries["ramsize"]:value()
  local n, u = v:match("(%d+)(%a?)")
  n = tonumber(n)
  if u == "M" or u == "m" then n = n * 1024 * 1024 elseif u == "K" or u == "k" then n = n * 1024 end
  return n
end
function mouse_to(h, v)
  mem:write_u16(0x828, v); mem:write_u16(0x82A, h)
  mem:write_u16(0x82C, v); mem:write_u16(0x82E, h)
  mem:write_u8(0x8CE, mem:read_u8(0x8CF))
end
OUT = os.getenv("RUN_DIR") or "."
local wake, co = 0, nil
function now() return manager.machine.time:as_double() end
function sleep(s) wake = now() + s; coroutine.yield() end
function run(f)
  co = coroutine.create(f)
  sub = emu.add_machine_frame_notifier(function()
    if co and now() >= wake and coroutine.status(co) ~= "dead" then
      local ok, err = coroutine.resume(co)
      if not ok then print("LUA ERROR " .. tostring(err)); manager.machine:exit() end
    end
  end)
end
function finish(ok) print(ok and "RESULT PASS" or "RESULT FAIL"); manager.machine:exit() end
B = nil
function find_block(timeout)
  local t_end = now() + (timeout or 120)
  while now() < t_end do
    local top = ram_top()
    for a = 0x1000, top - 16, 2 do
      if mem:read_u32(a) == 0x47534442 and mem:read_u32(a + 8) == 0xB8ACBBBD then B = a; return a end
    end
    sleep(2)
  end
end
function u32(o) return mem:read_u32(B + o) end
function i32(o) return mem:read_i32(B + o) end
function i16(o) return mem:read_i16(B + o) end
function w32(o, v) mem:write_u32(B + o, v) end
function canvas() return { top = i16(40), left = i16(42), bottom = i16(44), right = i16(46) } end
function wait_state(s, timeout)
  local t_end = now() + (timeout or 60)
  while now() < t_end do if u32(12) == s then return true end sleep(0.25) end
  return false
end
function shot(name) screen:snapshot(name .. ".png") end
-- drag through canvas points {x, y} (0..255)
function draw(pts, step)
  local c = canvas()
  mouse_to(c.left + pts[1][1], c.top + pts[1][2]); sleep(0.1)
  btn:set_value(1); sleep(0.08)
  for i = 2, #pts do mouse_to(c.left + pts[i][1], c.top + pts[i][2]); sleep(step or 0.04) end
  sleep(0.06); btn:clear_value(); sleep(0.15)
end
local KEYS = {}
for tag, port in pairs(manager.machine.ioport.ports) do
  if tag:find("kbd") or tag:find("KEY") then
    for name, f in pairs(port.fields) do
      local a, b = name:match("^(%S)  ?(%S)$")
      if a and not KEYS[a] then KEYS[a] = { f = f }; KEYS[b] = { f = f, shift = true } end
      KEYS[name] = KEYS[name] or { f = f }
      if name:find("^Command") then KEYS["Command"] = { f = f } end
    end
  end
end
KEYS["\r"] = KEYS["Return"]
function key(name)
  local k = KEYS[name]
  if not k then print("no key " .. name); return end
  k.f:set_value(1); sleep(0.08); k.f:clear_value(); sleep(0.15)
end
function mailbox(c, a)
  local d0 = u32(72)
  w32(68, a or 0); w32(64, c)
  local t_end = now() + 10
  while u32(72) == d0 and now() < t_end do sleep(0.1) end
end
-- a closed polygon (circle) with n points around (cx, cy), radius r
function ring(cx, cy, r, n, a0, a1)
  local p = {}
  a0 = a0 or 0; a1 = a1 or 2 * math.pi
  for i = 0, n do
    local a = a0 + (a1 - a0) * i / n
    p[#p + 1] = { math.floor(cx + r * math.cos(a) + 0.5), math.floor(cy + r * math.sin(a) + 0.5) }
  end
  return p
end

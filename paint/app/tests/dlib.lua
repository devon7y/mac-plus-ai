-- Helpers for Doodle's MAME tests: sequential test scripts (coroutines), the debug block,
-- mouse clicks and drags, typing, menu shortcuts, and dumping the page bitmap.
-- Works on the Plus (macplus) and the SE (macsefd, ADB): finds the mouse button and keys by name.
mem = manager.machine.devices[":maincpu"].spaces["program"]
screen = manager.machine.screens[":screen"]
for tag, port in pairs(manager.machine.ioport.ports) do
  for name, f in pairs(port.fields) do
    if name == "Mouse Button" or name == "Mouse Button 0" then btn = f end
  end
end
function ram_top()        -- physical RAM size, from MAME's ramsize option (as toolchain/se7/se_lib.lua)
  local v = manager.options.entries["ramsize"]:value()
  local n, u = v:match("(%d+)(%a?)")
  n = tonumber(n)
  if u == "M" or u == "m" then n = n * 1024 * 1024 elseif u == "K" or u == "k" then n = n * 1024 end
  return n
end
function mouse_to(h, v)   -- absolute cursor move (global coordinates), as emu/scripts/lib.lua
  mem:write_u16(0x828, v); mem:write_u16(0x82A, h)
  mem:write_u16(0x82C, v); mem:write_u16(0x82E, h)
  mem:write_u8(0x8CE, mem:read_u8(0x8CF))
end
OUT = os.getenv("TEST_OUT") or os.getenv("RUN_DIR") or "."
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

function finish(ok)
  print(ok and "RESULT PASS" or "RESULT FAIL")
  manager.machine:exit()
end

-- debug block ('DOOD', version, ~magic), scanned in 2-byte steps
B = nil
function find_block(timeout)
  local t_end = now() + (tonumber(os.getenv("BLOCK_WAIT") or "0") > 0 and tonumber(os.getenv("BLOCK_WAIT")) or timeout or 120)
  while now() < t_end do
    local top = ram_top()                               -- (MemTop isn't RAM top under System 7)
    for a = 0x1000, top - 16, 2 do
      if mem:read_u32(a) == 0x444F4F44 and mem:read_u32(a + 8) == 0xBBB0B0BB then B = a; return a end
    end
    sleep(2)
  end
end
function u32(o) return mem:read_u32(B + o) end
function i16(o) return mem:read_i16(B + o) end
function w32(o, v) mem:write_u32(B + o, v) end
function rect(o) return { top = i16(o), left = i16(o + 2), bottom = i16(o + 4), right = i16(o + 6) } end

function wait_ready(timeout)
  local t_end = now() + (timeout or 60)
  while now() < t_end do
    if u32(12) == 1 then return true end
    sleep(0.5)
  end
  return false
end

-- layout (screen coordinates), derived from the canvas rectangle in the block
L = {}
function layout()
  L.canvas = rect(32); L.frame = rect(40)
  L.ox = L.canvas.left - 48
end
function tool_xy(i) return L.ox + 2 + (i % 2) * 22 + 11, 24 + math.floor(i / 2) * 20 + 10 end
function finish_xy() return L.ox + 24, 24 + 120 + 6 + 10 end
function size_xy(i) return L.ox + 24, 24 + 120 + 6 + 20 + 8 + i * 12 + 6 end
function pat_xy(i) return L.canvas.left + (i % 19) * 17 + 8, L.canvas.bottom + 6 + math.floor(i / 19) * 13 + 6 end
-- frame coordinates (0..255) -> screen
function fxy(x, y) return L.frame.left + x, L.frame.top + y end

function click(h, v)
  mouse_to(h, v); sleep(0.1)
  btn:set_value(1); sleep(0.15)
  btn:clear_value(); sleep(0.25)
end
function dclick(h, v)
  mouse_to(h, v); sleep(0.1)
  btn:set_value(1); sleep(0.08); btn:clear_value(); sleep(0.08)
  btn:set_value(1); sleep(0.08); btn:clear_value(); sleep(0.4)
end
-- drag through a list of {h, v} screen points
function drag(pts, step)
  mouse_to(pts[1][1], pts[1][2]); sleep(0.1)
  btn:set_value(1); sleep(0.1)
  for i = 2, #pts do mouse_to(pts[i][1], pts[i][2]); sleep(step or 0.05) end
  sleep(0.1); btn:clear_value(); sleep(0.3)
end
function hold(h, v, secs)
  mouse_to(h, v); sleep(0.1); btn:set_value(1); sleep(secs); btn:clear_value(); sleep(0.3)
end

-- keyboard
local KEYS = {}
for tag, port in pairs(manager.machine.ioport.ports) do
  if tag:find("kbd") or tag:find("KEY") then
    for name, f in pairs(port.fields) do
      local a, b = name:match("^(%S)  ?(%S)$")
      if a and not KEYS[a] then KEYS[a] = { f = f }; KEYS[b] = { f = f, shift = true } end
      KEYS[name] = KEYS[name] or { f = f }
      if name:find("^Command") then KEYS["Command"] = { f = f } end
      if name == "Shift" or name == "Left Shift" then KEYS["Shift"] = { f = f } end
    end
  end
end
KEYS[" "] = KEYS["Space"]; KEYS["\r"] = KEYS["Return"]; KEYS["\b"] = KEYS["Backspace"]
local function field(n) return KEYS[n] and KEYS[n].f end
function key(name, mods)
  local k = KEYS[name]
  if not k then print("no key " .. name); return end
  local held = {}
  if k.shift then held[#held + 1] = field("Shift") end
  for _, m in ipairs(mods or {}) do held[#held + 1] = field(m) end
  for _, f in ipairs(held) do f:set_value(1) end
  sleep(0.05)
  k.f:set_value(1); sleep(0.08); k.f:clear_value(); sleep(0.05)
  for _, f in ipairs(held) do f:clear_value() end
  sleep(0.1)
end
function type_text(s) for i = 1, #s do key(s:sub(i, i)) end end
function cmd_key(c) key(c, { "Command" }) end

-- pick a menu item with the mouse: press on the title at x = title_h, drag down to item n, release
function menu_pick(title_h, n, shot_name)
  mouse_to(title_h, 10); sleep(0.1)
  btn:set_value(1); sleep(0.4)
  mouse_to(title_h + 8, 20 + (n - 1) * 16 + 8); sleep(0.4)
  if shot_name then shot(shot_name) end
  btn:clear_value(); sleep(0.5)
end
function menu_pick_y(title_h, y, shot_name)       -- same, by the item's y (System 7 icon items are taller)
  mouse_to(title_h, 10); sleep(0.1)
  btn:set_value(1); sleep(0.4)
  mouse_to(title_h + 8, y); sleep(0.4)
  if shot_name then shot(shot_name) end
  btn:clear_value(); sleep(0.5)
end
-- x (screen) of the i-th menu title (1 = Apple), from the Menu Manager's MenuList ($A1C):
-- 6-byte header (lastMenu offset, lastRight, mbResID), then (MenuHandle, menuLeft) per menu
function menu_x(i)
  local ml = mem:read_u32(mem:read_u32(0xA1C) & 0xFFFFFF) & 0xFFFFFF
  return mem:read_i16(ml + 6 + 6 * (i - 1) + 4) + 12
end

-- page bitmap (576 x 720, 72 bytes per row) -> file
function dump_doc(name)
  local base = u32(24)
  local f = io.open(OUT .. "/" .. name .. ".bin", "wb")
  local chunk = {}
  for i = 0, 51839 do
    chunk[#chunk + 1] = string.char(mem:read_u8(base + i))
    if #chunk == 4096 then f:write(table.concat(chunk)); chunk = {} end
  end
  f:write(table.concat(chunk)); f:close()
end
function dump_mem(name, addr, size)
  local f = io.open(OUT .. "/" .. name .. ".bin", "wb")
  local chunk = {}
  for i = 0, size - 1 do
    chunk[#chunk + 1] = string.char(mem:read_u8(addr + i))
    if #chunk == 4096 then f:write(table.concat(chunk)); chunk = {} end
  end
  f:write(table.concat(chunk)); f:close()
end
function shot(name) screen:snapshot(name .. ".png") end
-- count black pixels of the page in a rectangle given in document coordinates
function black(x0, y0, x1, y1)
  local base, n = u32(24), 0
  for y = y0, y1 - 1 do
    for x = x0, x1 - 1 do
      if (mem:read_u8(base + y * 72 + (x >> 3)) >> (7 - (x & 7))) & 1 == 1 then n = n + 1 end
    end
  end
  return n
end
-- document coordinates of a screen point
function dxy(h, v) return h - L.canvas.left, v - L.canvas.top end

-- mailbox: write args then the command, wait until cmdDone increments
function mailbox(c, a0, a1, a2, a3, timeout)
  local done0 = u32(148)
  w32(132, a0 or 0); w32(136, a1 or 0); w32(140, a2 or 0); w32(144, a3 or 0)
  w32(128, c)
  local t_end = now() + (timeout or 60)
  while u32(148) == done0 and now() < t_end do sleep(0.1) end
  return u32(152)
end

-- wait until the app has committed n more operations (ops counter at +48)
function wait_ops(ops0, n, timeout)
  local t_end = now() + (timeout or 10)
  while u32(48) < ops0 + (n or 1) and now() < t_end do sleep(0.1) end
  return u32(48) >= ops0 + (n or 1)
end

CHECKS, FAILED = 0, 0
function check(name, cond, info)
  CHECKS = CHECKS + 1
  if not cond then FAILED = FAILED + 1 end
  print(string.format("CHECK %-40s %s %s", name, cond and "ok" or "FAIL", info or ""))
end

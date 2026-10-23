-- Helpers for Dream's MAME tests (copied from paint/app/tests/dlib.lua): sequential test scripts (coroutines), the debug block,
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

-- debug block ('DREM', version, ~magic), scanned in 2-byte steps
B = nil
function find_block(timeout)
  -- The block is a global of the app (A5 world, between ApplZone $2AA and BufPtr $10C); scan that
  -- range first, the whole RAM as a fallback; require magic, version 1 and ~magic.
  local t_end = now() + (tonumber(os.getenv("BLOCK_WAIT") or "0") > 0 and tonumber(os.getenv("BLOCK_WAIT")) or timeout or 120)
  local function try(lo, hi)
    for a = lo, hi - 16, 2 do
      if mem:read_u32(a) == 0x4452454D and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == 0xBBADBAB2 then return a end
    end
  end
  while now() < t_end do
    local lo, hi = mem:read_u32(0x2AA) & 0xFFFFFE, mem:read_u32(0x10C) & 0xFFFFFE
    local a = nil
    if lo >= 0x1000 and hi > lo and hi <= ram_top() then a = try(lo, hi) end
    a = a or try(0x1000, ram_top())
    if a then B = a; return a end
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
    if u32(12) >= 2 then return true end
    sleep(0.5)
  end
  return false
end


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


function shot(name)
  manager.machine.video:snapshot()
  print("SHOT " .. name)
end
function dump_mem(name, addr, n)
  local f = io.open(OUT .. "/" .. name .. ".bin", "wb")
  local chunk = {}
  for x = 0, n - 1 do
    chunk[#chunk + 1] = string.char(mem:read_u8(addr + x))
    if #chunk == 4096 then f:write(table.concat(chunk)); chunk = {} end
  end
  f:write(table.concat(chunk)); f:close()
end
CHECKS, FAILED = 0, 0
function check(what, ok, detail)
  CHECKS = CHECKS + 1
  if not ok then FAILED = FAILED + 1 end
  print(string.format("CHECK %s: %s%s", ok and "ok" or "FAIL", what, detail and (" (" .. tostring(detail) .. ")") or ""))
end

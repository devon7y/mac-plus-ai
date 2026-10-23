-- On-screen keyboard test (PixelSE only; the Plus build has no on-screen keyboard), mouse only:
-- the keyboard is open when loading finishes; Close; reopen with Edit > Keyboard...; click
-- "hi pixelz", Delete, Shift on/off, Send; wait for the reply; Close; reopen with the Keys button;
-- Close. Needs a header-only "Pixel Test" script (fixed seed): the app reaches state 4, then runs
-- interactively. The layout comes from the app's debug block (geo at +208: kbArea, kbButton,
-- inputBox, area as top, left, bottom, right); keypos mirrors kbKey() in main.c.
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local p = manager.machine.ioport.ports
local btn = (p[":macadb:MOUSE0"] and p[":macadb:MOUSE0"].fields["Mouse Button 0"]) or p[":MOUSE0"].fields["Mouse Button"]
local MAGIC = 0x50584C21
local rows = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?"}
local bottom = {Shift = 0, ["!"] = 1, ["-"] = 2, space = 3, Delete = 4, Send = 5, Close = 6}
local cum = {0, 48, 78, 108, 228, 280, 332, 384}
local KB_MX, KB_GAP, KB_TOP, KB_KH, KB_PITCH = 6, 5, 5, 18, 22      -- main.c
local function mouse_to(h, v)
  mem:write_u16(0x828, v); mem:write_u16(0x82A, h); mem:write_u16(0x82C, v); mem:write_u16(0x82E, h)
  mem:write_u8(0x8CE, mem:read_u8(0x8CF))
end
local base, next_scan, q, wait, turns0 = nil, 30, {}, 0, 0
local function f(off) local v = mem:read_u32(base + off); if v >= 0x80000000 then v = v - 0x100000000 end; return v end
local function g(i) local v = mem:read_u16(base + 208 + 2 * i); if v >= 0x8000 then v = v - 0x10000 end; return v end
local function keypos(k)            -- the centre of key k (as kbKey: kbArea = geo 0..3)
  local t, l, r = g(0), g(1), g(3)
  local w, x0 = r - l - 2 * KB_MX + KB_GAP, l + KB_MX
  local a, b, top
  if bottom[k] then
    local i = bottom[k]
    a, b, top = w * cum[i + 1] // 384, w * cum[i + 2] // 384, t + KB_TOP + 4 * KB_PITCH
  else
    for ri, s in ipairs(rows) do
      local c = s:find(k, 1, true)
      if c then a, b, top = w * (c - 1) // 10, w * c // 10, t + KB_TOP + (ri - 1) * KB_PITCH end
    end
  end
  return x0 + (a + b - KB_GAP) // 2, top + KB_KH // 2
end
local function shot(name) manager.machine.video:snapshot(); print("SHOT " .. name) end
local function add(dt, fn) q[#q + 1] = {dt, fn} end
local function click_at(fn)       -- click where fn() says when the click starts (the layout may have moved)
  local h, v
  add(0.3, function() h, v = fn(); mouse_to(h, v) end)
  add(0.15, function() btn:set_value(1) end)
  add(0.5, function() btn:clear_value() end)
end
local function key(k) click_at(function() return keypos(k) end) end
local function keys_button() click_at(function() return (g(5) + g(7)) // 2, (g(4) + g(6)) // 2 end) end
local function until_(cond) q[#q + 1] = {0, nil, cond} end
-- the plan
until_(function() return base and f(12) == 4 end)
add(1, function() shot("kb0_start") end)                      -- open by itself after loading
key("Close")
add(1.5, function() shot("kb0b_closed") end)
add(0.3, function() mouse_to(94, 8) end)                      -- Edit menu title
add(0.3, function() btn:set_value(1) end)
add(0.8, function() mouse_to(110, 28) end)                    -- Keyboard...
add(0.8, function() btn:clear_value() end)
add(0.6, function() shot("kb1_menu_open_0.6s") end)
add(0.9, function() end)
for _, k in ipairs({"h", "i", "space", "p", "i", "x", "e", "l", "z"}) do key(k) end
add(1, function() shot("kb2_typed_z") end)
key("Delete")
key("Shift")
add(1, function() shot("kb3_shift_on") end)
key("Shift")
add(1, function() shot("kb4_shift_off_hi_pixel") end)
add(0, function() turns0 = f(68) end)
key("Send")
add(4, function() shot("kb5_thinking") end)
until_(function() return f(68) > turns0 and f(12) == 2 end)
add(1, function() shot("kb6_reply") end)
key("Close")
add(1.5, function() shot("kb7_closed") end)
keys_button()
add(0.6, function() shot("kb8_reopened_by_button_0.6s") end)
add(0.9, function() end)
key("Close")
add(1.5, function()
  shot("kb9_closed_again")
  local n, addr, t = f(36), mem:read_u32(base + 60), {}
  for i = 0, n - 1 do local v = mem:read_u16(addr + 2 * i); if v >= 0x8000 then v = v - 0x10000 end; t[#t + 1] = v end
  local fh = io.open((os.getenv("RUN_DIR") or ".") .. "/tokens.txt", "w"); fh:write(table.concat(t, " ") .. "\n"); fh:close()
  print("TOKENS " .. table.concat(t, " "))
  print(string.format("KBTICKS last keyboard draw=%d ticks, last full redraw=%d; kbArea t=%d l=%d b=%d r=%d; "
    .. "Keys t=%d l=%d b=%d r=%d", f(192), f(196), g(0), g(1), g(2), g(3), g(4), g(5), g(6), g(7)))
  print("RESULT KB DONE"); manager.machine:exit()
end)
local i, next_diag = 1, 100
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if t > 2500 then print("RESULT TIMEOUT"); manager.machine:exit() return end
  if t >= next_diag then
    next_diag = t + 100
    print(string.format("DIAG t=%.0f base=%s state=%s step=%d/%d", t, base and string.format("%06X", base) or "nil",
      base and tostring(f(12)) or "-", i, #q))
  end
  if not base then
    if t >= next_scan then
      next_scan = t + 10
      for a = 0x8000, 0x3FFFF0, 2 do
        if mem:read_u16(a) == 0x5058 and mem:read_u32(a) == MAGIC and mem:read_u32(a + 8) == 0xAFA7B3DE then base = a break end
      end
    end
    return
  end
  if t < wait or i > #q then return end
  local s = q[i]
  if s[3] then if s[3]() then i = i + 1 end return end
  s[2]()
  wait = t + s[1]
  i = i + 1
end)

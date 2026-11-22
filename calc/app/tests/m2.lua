-- Scribble Calc M2 check (MAME, Plus or SE):
--   1. classifier test vectors through the mailbox: the Mac's scores must equal the Python reference;
--      timing of 20 classifications;
--   2. handwritten lines drawn with the mouse (strokes from make_mouse_test.py): the Mac reads them
--      and writes the answer; the captured strokes are dumped for the Python cross-check.
-- Env: CALC_IN (input file), TEST_OUT (output folder, default $RUN_DIR), START_AT (emulated s).
mem = manager.machine.devices[":maincpu"].spaces["program"]
screen = manager.machine.screens[":screen"]
for tag, port in pairs(manager.machine.ioport.ports) do
  for name, f in pairs(port.fields) do
    if name == "Mouse Button" or name == "Mouse Button 0" then btn = f end
  end
end
local OUT = os.getenv("TEST_OUT") or os.getenv("RUN_DIR") or "."
local IN = os.getenv("CALC_IN")
local wake, co = 0, nil
function now() return manager.machine.time:as_double() end
function sleep(s) wake = now() + s; coroutine.yield() end
function frames(n) sleep(n / 60.15 - 0.001) end
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
function shot(name) screen:snapshot(name .. ".png") end
local B
function u32(o) return mem:read_u32(B + o) end
function i16(o) return mem:read_i16(B + o) end
function w32(o, v) mem:write_u32(B + o, v) end

local function find_block(timeout)
  local t_end = now() + timeout
  while now() < t_end do
    for a = 0x1000, ram_top() - 300, 2 do
      if mem:read_u32(a) == 0x5343414C and mem:read_u32(a + 8) == 0xACBCBEB3 then return a end
    end
    sleep(2)
  end
end

local function mailbox(cmd, arg)
  local d0 = u32(144)
  w32(140, arg or 0); w32(136, cmd)
  local t_end = now() + 30
  while u32(144) == d0 and now() < t_end do frames(1) end
  return u32(148)
end

local function read_str(o, n)
  local s = ""
  for i = 0, n - 1 do
    local c = mem:read_u8(B + o + i)
    if c == 0 then break end
    s = s .. string.char(c)
  end
  return s
end

local NAMES = { [0] = "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "+", "-", "x", "/", "=", "(", ")", "." }

local KEYS = {}
for tag, port in pairs(manager.machine.ioport.ports) do
  if tag:find("kbd") or tag:find("KEY") then
    for name, f in pairs(port.fields) do
      local a, b = name:match("^(%S)  ?(%S)$")
      if a and not KEYS[a] then KEYS[a] = f end
      KEYS[name] = KEYS[name] or f
    end
  end
end
local function key(ch)
  local f = KEYS[ch]
  if not f then print("no key " .. ch) return end
  f:set_value(1); frames(8); f:clear_value(); frames(8)
end
local function click(h, v)
  mouse_to(h, v); frames(3)
  btn:set_value(1); frames(6)
  btn:clear_value(); frames(10)
end

local function finish(ok)
  print(ok and "RESULT PASS" or "RESULT FAIL")
  manager.machine:exit()
end

co = coroutine.create(function()
  B = find_block(90)
  if not B then print("no debug block"); finish(false) return end
  print(string.format("block at %06x", B))
  local t_end = now() + 30
  while u32(12) ~= 1 and now() < t_end do sleep(0.5) end
  local start_at = tonumber(os.getenv("START_AT") or "0")
  if now() < start_at then sleep(start_at - now()) end
  local paper = { top = i16(16), left = i16(18), bottom = i16(20), right = i16(22) }
  print(string.format("paper t%d l%d b%d r%d, freeMem %d", paper.top, paper.left, paper.bottom, paper.right, u32(132)))
  local f = io.open(IN, "r")
  local items = {}
  for line in f:lines() do items[#items + 1] = line end
  f:close()
  -- 1. raster test vectors
  local nr, bad = 0, 0
  for _, line in ipairs(items) do
    if line:sub(1, 1) == "R" then
      local hex, rest = line:match("^R (%x+) (.*)$")
      for i = 0, 71 do mem:write_u8(B + 152 + i, tonumber(hex:sub(2 * i + 1, 2 * i + 2), 16)) end
      mailbox(1, 1)
      local k, ok = 0, true
      for v in rest:gmatch("%-?%d+") do
        if i16(224 + 2 * k) ~= tonumber(v) then ok = false end
        k = k + 1
      end
      nr = nr + 1
      if not ok then bad = bad + 1 end
    end
  end
  print(string.format("CHECK classifier bit-exact: %d of %d rasters match", nr - bad, nr))
  local ticks = mailbox(1, 20)
  print(string.format("TIMING 20 classifications: %d ticks = %.1f ms per symbol", ticks, ticks / 20 / 60.15 * 1000))
  -- 2. handwritten lines
  local dump = io.open(OUT .. "/captured.txt", "w")
  local nline, results = 0, {}
  local cur = nil
  local function finish_line()
    if not cur then return end
    local t_end2 = now() + 25
    sleep(0.5)
    while now() < t_end2 do
      if i16(38) == 0 and i16(40) > 0 and (i16(42) >= 0) then break end
      sleep(0.25)
    end
    sleep(0.5)
    local n = i16(40)
    local toks = ""
    for i = 0, n - 1 do
      local t = mem:read_i8(B + 44 + i)
      if t >= 32 then toks = toks .. "^" .. (NAMES[t - 32] or "?") else toks = toks .. (NAMES[t] or "?") end
    end
    local res = read_str(92, 32)
    print(string.format("LINE %d truth %-14s read %-14s answer %s (err %d, %d strokes, %d symbols, %d classified in %d ticks)",
      nline, cur, toks, res, i16(42), i16(32), i16(34), u32(124), u32(128)))
    sleep(1.5)                           -- let the redraw finish before the screenshot
    shot(string.format("line%d", nline))
    results[#results + 1] = { truth = cur, read = toks, res = res }
    -- fix misread symbols the way a user would: click it under the paper, type the right one
    if nline == 1 and #toks == #cur and toks ~= cur then
      local strip_v = i16(24) + 10
      for i = 1, #cur do
        if toks:sub(i, i) ~= cur:sub(i, i) then
          local x = i16(286 + 2 * (i - 1))
          print(string.format("FIX symbol %d (%s read as %s): click at %d,%d and type %s", i, cur:sub(i, i), toks:sub(i, i), x, strip_v, cur:sub(i, i)))
          click(x, strip_v)
          local t_pop = now() + 5
          while i16(382) == 0 and now() < t_pop do sleep(0.1) end
          sleep(0.3)
          shot(string.format("line%d_popup", nline))
          local want = ({ ["+"] = 10, ["-"] = 11, x = 12, ["/"] = 13, ["="] = 14, ["("] = 15, [")"] = 16, ["."] = 17 })[cur:sub(i, i)]
            or tonumber(cur:sub(i, i))
          local top, left = i16(382), i16(384)
          for c = 0, 17 do
            if mem:read_u8(B + 386 + c) == want then
              click(left + 4 + (c % 7) * 26 + 13, top + 4 + (c // 7) * 20 + 10)
            end
          end
          sleep(1.0)
        end
      end
      local t_end3 = now() + 15
      local n2, t2
      repeat
        sleep(0.25)
        n2, t2 = i16(40), ""
        for i = 0, n2 - 1 do t2 = t2 .. (NAMES[mem:read_i8(B + 44 + i)] or "?") end
      until (t2 == cur and i16(38) == 0) or now() > t_end3
      sleep(0.5)
      res = read_str(92, 32)
      print(string.format("LINE %d after the fix: read %s answer %s", nline, t2, res))
      results[#results].fixed = res
      sleep(1.5)
      shot(string.format("line%d_fixed", nline))
    end
    -- dump the strokes the Mac captured
    local stk, pts, ns = u32(272), u32(276), i16(280)
    dump:write("E " .. cur .. "\n")
    for i = 0, ns - 1 do
      local a = stk + i * 56
      if mem:read_u8(a + 12) ~= 0 then
        local first, cnt = mem:read_i16(a), mem:read_i16(a + 2)
        local t = {}
        for j = 0, cnt - 1 do
          t[#t + 1] = mem:read_i16(pts + (first + j) * 4) .. " " .. mem:read_i16(pts + (first + j) * 4 + 2)
        end
        dump:write("S " .. table.concat(t, " ") .. "\n")
      end
    end
  end
  for _, line in ipairs(items) do
    local tag = line:sub(1, 1)
    if tag == "E" then
      finish_line()
      nline = nline + 1
      cur = line:sub(3)
      mailbox(2, 0)                      -- New Line
      sleep(0.8)
    elseif tag == "S" then
      local p = {}
      for v in line:sub(3):gmatch("%-?%d+") do p[#p + 1] = tonumber(v) end
      mouse_to(paper.left + p[1], paper.top + p[2]); frames(2)
      btn:set_value(1); frames(5)            -- the Plus polls the button in its VBL task
      for k = 3, #p, 2 do mouse_to(paper.left + p[k], paper.top + p[k + 1]); frames(1) end
      frames(1)
      btn:clear_value(); frames(8)
    end
  end
  finish_line()
  dump:close()
  local okl = 0
  for _, r in ipairs(results) do if r.read == r.truth then okl = okl + 1 end end
  print(string.format("CHECK lines read exactly: %d of %d", okl, #results))
  print(string.format("first line answer: %s", results[1] and results[1].res or "none"))
  local first = results[1] and (results[1].fixed or results[1].res)
  print("first line answer (after any fixes): " .. tostring(first))
  finish(bad == 0 and first == "90")
end)

sub = emu.add_machine_frame_notifier(function()
  if co and now() >= wake and coroutine.status(co) ~= "dead" then
    local ok, err = coroutine.resume(co)
    if not ok then print("LUA ERROR " .. tostring(err)); manager.machine:exit() end
  end
end)

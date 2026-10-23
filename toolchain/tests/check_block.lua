-- Generic checker for the toolchain test apps (cpp, segments). Each app writes a block:
--   +0 magic tag, +4 version (1), +8 ~magic, +12 state (1 = done), +16 passBits, +20 nChecks
-- Usage: TAG=SEGT emu/run_test.sh <disk> toolchain/tests/check_block.lua 90   (tags: CPPT SEGT MEMP CYCL)
--        (CHECK_AT=<s> to start looking later, e.g. 65 at RAM=4M; SHOT=<name> for the screenshot;
--         CLICKS="t:h:v,..." clicks at emulated times, e.g. to answer a dialog)
mem = manager.machine.devices[":maincpu"].spaces["program"]
screen = manager.machine.screens[":screen"]
local ports = manager.machine.ioport.ports
local btn = (ports[":macadb:MOUSE0"] and ports[":macadb:MOUSE0"].fields["Mouse Button 0"])
         or ports[":MOUSE0"].fields["Mouse Button"]
-- CLICKS="t:h:v,...": click at (h, v) at emulated second t (e.g. to answer a dialog)
local clicks = {}
for t, h, v in (os.getenv("CLICKS") or ""):gmatch("([%d%.]+):(%d+):(%d+)") do
  clicks[#clicks + 1] = {tonumber(t), tonumber(h), tonumber(v)}
end
local function do_clicks(t)
  for _, c in ipairs(clicks) do
    if not c.down and t >= c[1] then
      c.down = true
      mem:write_u16(0x828, c[3]); mem:write_u16(0x82A, c[2]); mem:write_u16(0x82C, c[3]); mem:write_u16(0x82E, c[2])
      mem:write_u8(0x8CE, mem:read_u8(0x8CF)); btn:set_value(1)
    elseif c.down and not c.up and t >= c[1] + 0.2 then
      c.up = true; btn:clear_value(); screen:snapshot(string.format("click_%d.png", math.floor(c[1])))
    end
  end
end
local function ram_top()        -- RAM size from MAME's -ramsize (MemTop at $108 isn't it under System 7)
  local n, u = manager.options.entries["ramsize"]:value():match("(%d+)(%a?)")
  return tonumber(n) * ((u == "M" or u == "m") and 1048576 or (u == "K" or u == "k") and 1024 or 1)
end
local tag = os.getenv("TAG") or "SEGT"
local MAGIC = 0
for i = 1, 4 do MAGIC = (MAGIC << 8) | tag:byte(i) end
local CHECK_AT = tonumber(os.getenv("CHECK_AT") or "30")
local SHOT = os.getenv("SHOT") or tag:lower()

local function scan()
  for a = ram_top() - 8, 0x1000, -2 do   -- step 2: A5 world is only word-aligned
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then return a end
  end
end

local next_scan, done = CHECK_AT, false
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  do_clicks(t)
  if done or t < next_scan then return end
  local b = scan()
  if not (b and mem:read_u32(b + 12) == 1) then
    if t > CHECK_AT + 120 then print("RESULT FAIL (block not found or not done: TIMEOUT)"); done = true; manager.machine:exit() end
    next_scan = t + 5
    return
  end
  local bits, n = mem:read_u32(b + 16), mem:read_u32(b + 20)
  print(string.format("'%s' block at %06x (t=%.1fs): passBits=%x of %d checks", tag, b, t, bits, n))
  if tag == "SEGT" then
    local got, sizes = {}, {}
    for i = 0, 5 do got[#got + 1] = string.format("%08x", mem:read_u32(b + 24 + 4 * i)) end
    for i = 0, 9 do local s = mem:read_i32(b + 60 + 4 * i); if s >= 0 then sizes[#sizes + 1] = string.format("CODE %d=%d", i, s) end end
    print("got " .. table.concat(got, " ") .. string.format(" expect %08x %08x %08x",
      mem:read_u32(b + 48), mem:read_u32(b + 52), mem:read_u32(b + 56)))
    print(table.concat(sizes, ", "))
    print(string.format("FreeMem before %d, all loaded %d, after UnloadSeg+MaxMem %d; ticks A %d B %d C %d",
      mem:read_u32(b + 100), mem:read_u32(b + 104), mem:read_u32(b + 108),
      mem:read_u32(b + 112), mem:read_u32(b + 116), mem:read_u32(b + 120)))
    print(string.format("table_sum %08x, 70 KB global array at %06x (A5 world), checksum %d",
      mem:read_u32(b + 124), mem:read_u32(b + 132), mem:read_u32(b + 128)))
  elseif tag == "CYCL" then
    local ri, rt, mi, mt = mem:read_u32(b + 24), mem:read_u32(b + 28), mem:read_u32(b + 32), mem:read_u32(b + 36)
    print(string.format("reg loop: %d x 18 cycles in %d ticks -> %.3f MHz effective (of 7.8336)", ri, rt, ri * 18 * 60.15 / rt / 1e6))
    print(string.format("mem loop: %d x 194 cycles in %d ticks -> %.3f MHz effective", mi, mt, mi * 194 * 60.15 / mt / 1e6))
  elseif tag == "MEMP" then
    local f = {"ProcessMgr", "processSize", "processFreeMem", "FreeMem", "MaxMem", "TempFreeMem", "TempMaxMem",
               "ApplLimit-ApplZone", "StackSpace"}
    local out = {}
    for i, name in ipairs(f) do out[#out + 1] = string.format("%s=%d", name, mem:read_i32(b + 24 + 4 * (i - 1))) end
    print(table.concat(out, " "))
    print(string.format("bounds=(%d,%d,%d,%d) MemTop=%06x BufPtr=%06x ApplZone=%06x A5=%06x", mem:read_i16(b + 60),
      mem:read_i16(b + 62), mem:read_i16(b + 64), mem:read_i16(b + 66), mem:read_u32(b + 68), mem:read_u32(b + 72),
      mem:read_u32(b + 76), mem:read_u32(b + 80)))
  else
    local v = {}
    for i = 0, n - 1 do v[#v + 1] = tostring(mem:read_i32(b + 24 + 4 * i)) end
    print("values " .. table.concat(v, " "))
  end
  screen:snapshot(SHOT .. ".png")
  print(bits == (1 << n) - 1 and n > 0 and "RESULT PASS" or "RESULT FAIL")
  done = true
  manager.machine:exit()
end)

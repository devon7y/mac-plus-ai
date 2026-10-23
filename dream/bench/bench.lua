-- Dream kernel benchmark harness (adapted from paint/bench/bench.lua).
-- Finds the results block tagged 'DRMB' (+4 = ~tag) inside the application heap
-- (ApplZone $2AA .. BufPtr $10C), serves dump requests (status 2: copy each requested buffer
-- to $BENCH_OUT/dump_<id>.bin, then status 0) and, when the program is done (status 1),
-- prints every entry and exits. BENCH_LIMIT = emulated-seconds timeout.
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local out = os.getenv("BENCH_OUT") or "."
local limit = tonumber(os.getenv("BENCH_LIMIT") or "3000")
local base = nil
local next_scan = tonumber(os.getenv("BENCH_SCAN_FROM") or "8")
local ndumps = 0
local TAG = 0x44524D42
local function scan()
  local lo = mem:read_u32(0x2AA) & 0xFFFFFE
  local hi = mem:read_u32(0x10C) & 0xFFFFFE
  if lo < 0x1000 or hi <= lo or hi > 0x400000 then return nil end
  for a = lo, hi - 8, 2 do
    if mem:read_u32(a) == TAG and mem:read_u32(a + 4) == (~TAG & 0xFFFFFFFF) then return a end
  end
end
local function tag(v)
  return string.char((v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255)
end
local function dump(addr, size, id)
  local f = io.open(string.format("%s/dump_%d.bin", out, id), "wb")
  local chunk = {}
  for x = 0, size - 1 do
    chunk[#chunk + 1] = string.char(mem:read_u8(addr + x))
    if #chunk == 4096 then f:write(table.concat(chunk)); chunk = {} end
  end
  f:write(table.concat(chunk))
  f:close()
  ndumps = ndumps + 1
end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not base then
    if t >= next_scan then
      base = scan(); next_scan = t + 2
      if base then print(string.format("results block at %06x (t=%.1fs)", base, t)) end
    end
    if t > limit then print("TIMEOUT (no results block)"); manager.machine:exit() end
    return
  end
  local st = mem:read_u32(base + 8)
  if st == 2 then
    local n = mem:read_u32(base + 16)
    for i = 0, n - 1 do
      local d = base + 20 + i * 12
      dump(mem:read_u32(d), mem:read_u32(d + 4), mem:read_u32(d + 8))
    end
    print(string.format("dumped %d buffers at t=%.1fs", n, t))
    mem:write_u32(base + 16, 0)
    mem:write_u32(base + 8, 0)
    return
  end
  if st ~= 1 then
    if t > limit then print("TIMEOUT"); manager.machine:exit() end
    return
  end
  local n = mem:read_u32(base + 12)
  print(string.format("done at t=%.1fs emulated, %d entries, %d dumps", t, n, ndumps))
  for i = 0, n - 1 do
    local e = base + 128 + i * 16
    print(string.format("ENTRY %s %d %d %d", tag(mem:read_u32(e)), mem:read_u32(e + 4), mem:read_u32(e + 8),
      mem:read_u32(e + 12)))
  end
  manager.machine:exit()
end)

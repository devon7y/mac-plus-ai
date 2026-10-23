-- Run inside MAME (macplus, 1 MB): find the benchmark's results block ('PNTB'), serve its
-- dump requests (status 2: copy each requested buffer to $BENCH_OUT/dump_<id>.bin, then set
-- status 0), and when it is done (status 1) print every entry and exit.
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local out = os.getenv("BENCH_OUT") or "."
local limit = tonumber(os.getenv("BENCH_LIMIT") or "1500")
local base = nil
local next_scan = 4
local ndumps = 0
local function scan()
  for a = 0x1000, (tonumber(os.getenv("BENCH_RAMTOP") or "1048576")) - 16, 2 do
    if mem:read_u32(a) == 0x504E5442 and mem:read_u32(a + 4) <= 2 and mem:read_u32(a + 8) < 1000 then return a end
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
    return
  end
  local st = mem:read_u32(base + 4)
  if st == 2 then
    local n = mem:read_u32(base + 12)
    for i = 0, n - 1 do
      local d = base + 16 + i * 12
      dump(mem:read_u32(d), mem:read_u32(d + 4), mem:read_u32(d + 8))
    end
    mem:write_u32(base + 12, 0)
    mem:write_u32(base + 4, 0)
    return
  end
  if st ~= 1 then
    if t > limit then print("TIMEOUT"); manager.machine:exit() end
    return
  end
  local n = mem:read_u32(base + 8)
  print(string.format("done at t=%.1fs emulated, %d entries, %d dumps", t, n, ndumps))
  for i = 0, n - 1 do
    local e = base + 512 + i * 16
    print(string.format("ENTRY %s %d %d %d", tag(mem:read_u32(e)), mem:read_u32(e + 4), mem:read_u32(e + 8),
      mem:read_u32(e + 12)))
  end
  manager.machine:exit()
end)

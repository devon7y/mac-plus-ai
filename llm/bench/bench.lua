-- Run inside MAME (macplus, 4 MB): wait for the benchmark's results block ('BNCH'),
-- print timing/fit entries, and dump test buffers to $BENCH_OUT/dump_<i>.bin.
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local out = os.getenv("BENCH_OUT") or "."
local base = nil
local next_scan = 30
local function scan()
  for a = 0x20000, 0x3F0000, 4 do
    if mem:read_u32(a) == 0x424E4348 and mem:read_u32(a + 4) <= 1 and mem:read_u32(a + 8) < 200 then return a end
  end
end
local function tag(v)
  return string.char((v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255)
end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not base then
    if t >= next_scan then
      base = scan(); next_scan = t + 10
      if base then print(string.format("results block at %06x (t=%.0fs)", base, t)) end
    end
    return
  end
  if mem:read_u32(base + 4) ~= 1 then
    if t > 900 then print("TIMEOUT"); manager.machine:exit() end
    return
  end
  local n = mem:read_u32(base + 8)
  print(string.format("done at t=%.1fs emulated, %d entries", t, n))
  for i = 0, n - 1 do
    local e = base + 12 + i * 16
    local tg, a, b, c = mem:read_u32(e), mem:read_u32(e + 4), mem:read_u32(e + 8), mem:read_u32(e + 12)
    local name = tag(tg)
    if name == "Dump" then
      local f = io.open(string.format("%s/dump_%d.bin", out, c), "wb")
      for x = 0, b - 1 do f:write(string.char(mem:read_u8(a + x))) end
      f:close()
    else
      print(string.format("ENTRY %s %d %d %d", name, a, b, c))
    end
  end
  manager.machine:exit()
end)

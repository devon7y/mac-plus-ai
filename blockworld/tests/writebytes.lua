-- Blockworld: write given bytes to a file on this disk (debug command 14, mode 5): FILE=<host file>,
-- OFF=<offset> LEN=<bytes, up to 65536, a multiple of 1024>, PIECE=<KB>. Prints the File Manager's result.
local cpu = manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local MAGIC, NOTMAGIC = 0x4257474D, 0xBDA8B8B2
local blk, next_scan, phase, t_go = nil, 30, 0, 0
local data = io.open(os.getenv("FILE"), "rb"):read("*a")
local off, len = tonumber(os.getenv("OFF") or "0"), tonumber(os.getenv("LEN") or "4096")
local function scan()
  for a = 0x1000, mem:read_u32(0x108) - 8, 2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == NOTMAGIC then return a end
  end
end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not blk then
    if t >= next_scan then blk = scan(); next_scan = t + 0.5 end
    if t > 200 then print("RESULT FAIL no game"); manager.machine:exit() end
    return
  end
  if mem:read_u32(blk + 8) ~= 2 or mem:read_u32(blk + 40) < 1 or t < t_go or mem:read_u16(blk + 162) ~= 0 then return end
  if phase == 0 then                         -- (the game's 64 KB buffer first)
    mem:write_u16(blk + 162, 16); phase = 0.5; t_go = t + 0.5
  elseif phase == 0.5 then
    local b = mem:read_u32(blk + 314)
    for i = 0, len - 1 do mem:write_u8(b + i, data:byte(off + i + 1)) end
    mem:write_u16(blk + 156, 12345)
    mem:write_u16(blk + 164, len // 1024); mem:write_u16(blk + 166, tonumber(os.getenv("PIECE") or "4")); mem:write_u16(blk + 168, 5 + tonumber(os.getenv("PAUSE") or "0"))
    mem:write_u16(blk + 162, 14)
    phase = 1; t_go = t + 1
  else
    print(string.format("RESULT bytes %d..%d of %s: error %d", off, off + len - 1, os.getenv("FILE"), mem:read_i16(blk + 156)))
    manager.machine:exit()
  end
end)

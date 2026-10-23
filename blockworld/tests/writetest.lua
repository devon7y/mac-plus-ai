-- Blockworld: can the game write files on this disk? Debug command 14 writes "Blockworld Test" of
-- each size in KB (SIZES="1,4,16,48"), in pieces of PIECE KB, and prints the File Manager result.
local cpu = manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local MAGIC, NOTMAGIC = 0x4257474D, 0xBDA8B8B2
local blk, next_scan, i, t_go = nil, 30, 0, 0
local sizes = {}
for v in (os.getenv("SIZES") or "1,4,16,48"):gmatch("%d+") do sizes[#sizes + 1] = tonumber(v) end
local piece = tonumber(os.getenv("PIECE") or "64")
local heads = tonumber(os.getenv("HEADS") or "0")
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
  if i > 0 then print(string.format("RESULT write %d KB in %d KB pieces (headers first %d): error %d", sizes[i], piece, heads, mem:read_i16(blk + 156))) end
  i = i + 1
  if i > #sizes then manager.machine:exit(); return end
  mem:write_u16(blk + 156, 12345)
  mem:write_u16(blk + 164, sizes[i]); mem:write_u16(blk + 166, piece); mem:write_u16(blk + 168, heads); mem:write_u16(blk + 162, 14)
  t_go = t + 1
end)

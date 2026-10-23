-- PC-sampling profiler for Rail Runner's BENCH build: samples the 68000 PC once per emulated
-- video frame while the benchmark runs, writes $RUN_DIR/pc_samples.txt (one "pc anchor" pair
-- per line, anchor = runtime address of render_objects) for tests/profile.py.
dofile(os.getenv("EMU") .. "/scripts/lib.lua")
local MAGIC = 0x5252554E
local blk, next_scan = nil, 8
local cpu = manager.machine.devices[":maincpu"]
local f = io.open(os.getenv("RUN_DIR") .. "/pc_samples.txt", "w")
local n = 0
local function scan()
  local top = mem:read_u32(0x108)
  for a = top - 4, 0x1000, -2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then return a end
  end
end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not blk then
    if t >= next_scan then blk = scan(); next_scan = t + 4 end
    return
  end
  local mode = mem:read_i32(blk + 12)
  if mode == 2 and mem:read_i32(blk + 16) > 30 then
    f:write(string.format("%d %d %d\n", cpu.state["PC"].value, mem:read_u32(blk + 150), mem:read_u32(blk + 154)))
    n = n + 1
  end
  if mode == 4 or t > 400 then
    f:close()
    print("samples " .. n .. ", fps " .. mem:read_i32(blk + 24) / 100)
    manager.machine:exit()
  end
end)

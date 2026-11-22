-- TuneBench in MAME: wait for the 'TUNB' block, print ticks per tune and token, dump the tokens.
--   SLOW=1.5 emu/run_test.sh tune/build/bench_disk.img tune/app/bench/bench.lua 900
mem = manager.machine.devices[":maincpu"].spaces["program"]
local function ram_top()
  local n, u = manager.options.entries["ramsize"]:value():match("(%d+)(%a?)")
  return tonumber(n) * ((u == "M" or u == "m") and 1048576 or (u == "K" or u == "k") and 1024 or 1)
end
local MAGIC = 0x54554E42
local NT, MAXT = 4, 400
local blk, next_scan = nil, 15
local function scan()
  for a = ram_top() - 8, 0x1000, -2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then
      return a
    end
  end
end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if t < next_scan then return end
  next_scan = t + 5
  if not blk then blk = scan(); if blk then print(string.format("block at %06x t=%.0f", blk, t)) end; return end
  local st = mem:read_i32(blk + 12)
  if st == 2 or st < 0 then
    print(string.format("state %d freeMem %d", st, mem:read_u32(blk + 20)))
    local f = io.open(os.getenv("RUN_DIR") .. "/bench_tokens.txt", "w")
    local base = blk + 64
    local tot_ticks, tot_tok = 0, 0
    for i = 0, mem:read_u32(blk + 16) - 1 do
      local a = base + i * (8 + 2 * MAXT)
      local ticks, n = mem:read_u32(a), mem:read_u16(a + 4)
      local toks = {}
      for k = 0, n - 1 do toks[#toks + 1] = tostring(mem:read_u16(a + 8 + 2 * k)) end
      f:write(table.concat(toks, " ") .. "\n")
      print(string.format("tune %d: %d tokens in %d ticks = %.3f s/token", i, n, ticks, ticks / 60.15 / (n - 1)))
      tot_ticks = tot_ticks + ticks; tot_tok = tot_tok + n - 1
    end
    f:close()
    print(string.format("TOTAL %.3f s/token (%d tokens)", tot_ticks / 60.15 / tot_tok, tot_tok))
    local kt, kr = mem:read_u32(blk + 24), mem:read_u32(blk + 28)
    print(string.format("k_mv recurrent: %.2f ms per call", kt / 60.15 / kr * 1000))
    local names = {"tg_feed", "tg_sample", "k_gru", "gram_allowed", "gram_lookback", "k_tab5", "k_scale", "k_mv out"}
    for i = 0, 7 do
      print(string.format("  %-14s %.2f ms", names[i + 1], mem:read_u32(blk + 32 + 4 * i) / 60.15 / 200 * 1000))
    end
    manager.machine:exit()
  end
end)

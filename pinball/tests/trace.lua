-- Instruction trace of the first micro-benchmark (empty world, 1 ball) in a HASHTEST build:
-- needs MAME_ARGS="-debug -debugger none". Writes $RUN_DIR/trace.log for TRACE_SECS (0.03 s).
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local MAGIC = 0x4E50424C
local blk, next_scan, tracing, t_on = nil, 10, false, 0
local dbg = manager.machine.debugger
local secs = tonumber(os.getenv("TRACE_SECS") or "0.03")
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not blk then
    if t < next_scan then return end
    next_scan = t + 1
    for a = 0x100000 - 4, 0x1000, -2 do
      if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then blk = a; break end
    end
    return
  end
  if not seen and mem:read_u32(blk + 68) ~= 0 then seen = t end
  if not tracing and seen and t - seen > tonumber(os.getenv("TRACE_DELAY") or "0.15") and mem:read_i32(blk + 108) == 0 then
    dbg:command("trace " .. os.getenv("RUN_DIR") .. "/trace.log,0,noloop")
    tracing = true
    t_on = t
    print(string.format("tracing from t=%.2f, anchor %08x", t, mem:read_u32(blk + 104)))
  elseif tracing and t - t_on > secs then
    dbg:command("trace off")
    print("trace done")
    manager.machine:exit()
  end
end)

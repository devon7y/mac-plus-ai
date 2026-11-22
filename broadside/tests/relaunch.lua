-- Broadside in MAME: quit (mailbox 9) and check the app comes back (the floppies name it as the
-- shell), with MacinTalk opened again when the disk has it. Prints speech before/after.
--   emu/run_test.sh broadside/build/test/broadside_talk.img broadside/tests/relaunch.lua 120
mem = manager.machine.devices[":maincpu"].spaces["program"]
local function ram_top()
  local n, u = manager.options.entries["ramsize"]:value():match("(%d+)(%a?)")
  return tonumber(n) * ((u == "M" or u == "m") and 1048576 or (u == "K" or u == "k") and 1024 or 1)
end
local MAGIC = 0x42524453
local function scan()
  for a = ram_top() - 8, 0x1000, -2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then return a end
  end
end
local oldLoops, fresh = 0, false
local blk, step, nextT, sp0 = nil, 1, tonumber(os.getenv("CHECK_AT") or "25"), 0
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if t < nextT then return end
  nextT = t + (step == 2 and 0.2 or 1)
  if step == 1 then
    blk = scan()
    if blk and mem:read_u32(blk + 16) > 50 then
      sp0 = mem:read_u32(blk + 84)
      oldLoops = mem:read_u32(blk + 16)
      print(string.format("first launch: speech %d, t=%.0f", sp0, t))
      mem:write_u32(blk + 20, 9); step = 2; nextT = t + 2
    end
  elseif step == 2 then
    -- the old block is gone (the heap was reinitialised) or a new one has loops < old
    local b = scan()
    if b and mem:read_u32(b + 16) < oldLoops then fresh = true end
    if fresh and b and mem:read_u32(b + 16) > 50 and mem:read_u32(b + 20) == 0 then
      print(string.format("relaunched: speech %d, t=%.0f", mem:read_u32(b + 84), t))
      print((mem:read_u32(b + 84) == sp0) and "RESULT PASS" or "RESULT FAIL")
      manager.machine:exit()
    end
    if t > 110 then print("RESULT FAIL (no relaunch)"); manager.machine:exit() end
  end
end)

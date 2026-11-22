-- Orbit Duel M2, one Mac: open AppleTalk (turning it on if the PRAM says it is off), open a DDP
-- socket, register "<name>:Orbit Duel@*", look the type up, and (if .MPP is version 48 or later,
-- e.g. System 7) switch on self-send and send a datagram to our own socket.
--   emu/run_test.sh duel/build/orbitduel.img duel/tests/atalk_one.lua 120
--   MACHINE=macsefd RAM=4M HARDDISK=duel/build/orbitduel_se7.hd CHECK_AT=60 emu/run_test.sh none duel/tests/atalk_one.lua 200
mem = manager.machine.devices[":maincpu"].spaces["program"]
screen = manager.machine.screens[":screen"]
local MAGIC = 0x4F44554C
local check_at = tonumber(os.getenv("CHECK_AT") or "15")
local blk, phase, t_phase, asked = nil, "find", 0, false
local function scan()
  for a = 0x1000, 0x3FFFF0, 2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then return a end
  end
end
local function rd(o) return mem:read_i32(blk + o) end
local function wr(o, v) mem:write_u32(blk + o, v) end
local function report(t)
  print(string.format("t=%.1f screen %d link state %d | SPConfig/PortBUse %04x | .MPP open %d, version %d | node %d net %d socket %d | register %d | lookups %d, names %d, others %d | self-test bits %02x, loopback %d | err %d",
    t, rd(12), rd(88), rd(172), rd(132), rd(136), rd(140), rd(144), rd(148), rd(152), rd(160), rd(156), rd(176), rd(192), rd(168), rd(128)))
end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if phase == "find" then
    if t >= check_at then
      blk = scan()
      if blk and rd(12) == 1 then phase, t_phase = "title", t else check_at = t + 2 end
    end
  elseif phase == "title" and t > t_phase + 2 then
    wr(48, 78)                                  -- arg 78: AppleTalk self-test
    wr(44, 3)
    phase, t_phase = "at", t
    print(string.format("AppleTalk game requested at t=%.1f", t))
  elseif phase == "at" then
    local st = rd(88)
    if st == 1 and not asked then               -- LS_ASK_ON: "turn AppleTalk on?" -> yes
      asked = true
      screen:snapshot("ask_on.png")
      report(t)
      wr(44, 3)
    end
    if t > t_phase + 5 and math.floor(t - t_phase) % 5 == 0 and math.floor((t - t_phase) * 60) % 300 == 0 then report(t) end
    if (st == 5 and rd(160) >= 3) or st == 2 or t > t_phase + 40 then
      report(t)
      screen:snapshot("atalk.png")
      local bits = rd(192)
      print(string.format("RESULT %s (bits: 1 .MPP open, 2 socket, 4 name registered, 8 own name found, 16 self-send on, 32 datagram looped back)",
        (bits & 7) == 7 and "PASS" or "FAIL"))
      manager.machine:exit()
    end
  end
end)

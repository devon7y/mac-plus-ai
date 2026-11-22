-- Orbit Duel: the standard keys through MAME's own keyboard (Plus keyboard), including a
-- rotate + thrust + fire combination (D + W + Shift: two ordinary keys and a modifier).
--   emu/run_test.sh duel/build/orbitduel.img duel/tests/keys.lua 90
mem = manager.machine.devices[":maincpu"].spaces["program"]
screen = manager.machine.screens[":screen"]
local MAGIC = 0x4F44554C
local ports = manager.machine.ioport.ports
local function field(want)
  for _, port in pairs(ports) do
    for name, f in pairs(port.fields) do
      if name == want or name:find("^" .. want:gsub("%p", "%%%0") .. "  ") then return f end
    end
  end
end
local K = {d = field("d"), a = field("a"), w = field("w"), s = field("s"), space = field("Space"),
           shift = field("Shift"), esc = field("`"), ret = field("Return")}
for k, f in pairs(K) do if not f then print("no key field " .. k) end end
local blk, phase, t0, step, fails = nil, "find", 0, 0, 0
local check_at = 12
local function scan()
  for a = 0x1000, 0xFFFF0, 2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then return a end
  end
end
local function rd(o) return mem:read_i32(blk + o) end
local function rd16(o) return mem:read_i16(blk + o) end
local function check(ok, what) print((ok and "ok   " or "FAIL ") .. what); if not ok then fails = fails + 1 end end
local a0, s0, j0, thr

-- {time after the game started, action}
local plan = {
  {1.5, function() a0 = rd16(236); K.d:set_value(1) end},
  {2.0, function() K.d:clear_value(); check(((a0 - rd16(236)) & 255) > 20 and ((a0 - rd16(236)) & 255) < 128, string.format("D turns right (angle %d -> %d)", a0, rd16(236))) end},
  {2.3, function() a0 = rd16(236); K.a:set_value(1) end},
  {2.8, function() K.a:clear_value(); check(((rd16(236) - a0) & 255) > 20 and ((rd16(236) - a0) & 255) < 128, string.format("A turns left (angle %d -> %d)", a0, rd16(236))) end},
  {3.0, function() K.w:set_value(1) end},
  {3.3, function() thr = rd16(238); end},
  {3.5, function() K.w:clear_value(); check(thr == 1, "W thrusts") end},
  {3.8, function() s0 = rd(244); K.space:set_value(1) end},
  {4.0, function() K.space:clear_value() end},
  {4.3, function() check(rd(244) > s0, string.format("Space fires (shots %d -> %d)", s0, rd(244))) end},
  {4.6, function() s0 = rd(244); K.shift:set_value(1) end},
  {4.8, function() K.shift:clear_value() end},
  {5.1, function() check(rd(244) > s0, "Shift fires") end},
  {5.4, function() j0 = rd(248); K.s:set_value(1) end},
  {5.6, function() K.s:clear_value() end},
  {5.9, function() check(rd(248) > j0, "S jumps to hyperspace") end},
  {8.0, function() a0 = rd16(236); s0 = rd(244); K.d:set_value(1); K.w:set_value(1); K.shift:set_value(1) end},
  {8.3, function() thr = rd16(238) end},
  {8.5, function()
      local turned = (a0 - rd16(236)) & 255
      K.d:clear_value(); K.w:clear_value(); K.shift:clear_value()
      check(turned > 10 and turned < 128 and thr == 1 and rd(244) > s0,
        string.format("D + W + Shift together: turned %d, thrust %d, shots +%d (input byte now %02x)", turned, thr, rd(244) - s0, rd16(254)))
    end},
  {9.5, function() screen:snapshot("keys.png"); K.ret:set_value(1) end},
  {9.7, function() K.ret:clear_value() end},
  {10.0, function() check(rd16(36) >= 0 and mem:read_i32(blk + 28) > 0, "Return pauses (game still there)") end},
  {10.5, function() K.esc:set_value(1) end},
  {10.7, function() K.esc:clear_value() end},
  {12.0, function() check(rd(12) == 1, "Esc (the Plus's ` key) goes back to the title") end},
}
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if phase == "find" then
    if t >= check_at then
      blk = scan()
      if blk and rd(12) == 1 then phase, t0 = "title", t else check_at = t + 2 end
    end
  elseif phase == "title" and t > t0 + 2 then
    mem:write_u32(blk + 52, 2)              -- keys fly the Needle, the Wedge idles
    mem:write_u32(blk + 44, 1)
    phase, t0 = "wait", t
  elseif phase == "wait" and rd(12) == 2 then
    phase, t0, step = "plan", t, 1
  elseif phase == "plan" then
    while step <= #plan and t - t0 >= plan[step][1] do plan[step][2](); step = step + 1 end
    if step > #plan then
      print(fails == 0 and "RESULT PASS" or ("RESULT FAIL " .. fails))
      manager.machine:exit()
    end
  end
  if t > 120 then print("RESULT FAIL (timeout in " .. phase .. ")"); manager.machine:exit() end
end)

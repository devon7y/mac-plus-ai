-- Neural Pinball on the SE floppy (macsefd, 4 MB, System 6.0.7): attract mode, a player's game,
-- plunger, a multiball (test hook) whose callout MacinTalk speaks if present; frame rates and the
-- callout count. Run with MAME_ARGS="-wavwrite <file>" to capture the sound.
--   SLOW=1.3 MACHINE=macsefd RAM=4M emu/run_test.sh pinball/build/game/pinball_se.img pinball/tests/se_check.lua 200
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local screen = manager.machine.screens[":screen"]
local ports = manager.machine.ioport.ports
local MAGIC = 0x4E50424C
local function find_field(names)
  for _, want in ipairs(names) do
    for _, port in pairs(ports) do for name, f in pairs(port.fields) do if name == want then return f end end end
  end
end
local function find_char(c)
  local pat = "^" .. c:lower():gsub("%p", "%%%0") .. "  "
  for _, port in pairs(ports) do for name, f in pairs(port.fields) do if name:find(pat) then return f end end end
end
local K = {Return = find_field({"Return"}), S = find_char("s"), A = find_char("a"), D = find_char("d")}
local blk, t0, next_scan = nil, nil, 30
local function rd(o) return mem:read_i32(blk + o) end
local function rd16(o) return mem:read_i16(blk + o) end
local res = {}
local function status(tag)
  local r = {tag = tag, fps = rd(36) / 100, tps = rd(40) / 100, mode = rd(12), spoken = rd16(150), speech = rd16(148)}
  res[#res + 1] = r
  print(string.format("[%s] t=%.1f mode %d score %d ball %d inplay %d fps %.1f ticks/s %.1f screen_x %d speech %d spoken %d",
    tag, manager.machine.time:as_double(), r.mode, rd(24), rd16(28), rd16(34), r.fps, r.tps, rd(92), r.speech, r.spoken))
end
local ev = {
  {6, function() screen:snapshot("se_attract.png"); status("attract"); K.Return:set_value(1) end},
  {6.2, function() K.Return:clear_value() end},
  {8, function() K.S:set_value(1) end},
  {8.9, function() K.S:clear_value() end},
  {14, function() screen:snapshot("se_play.png"); status("play") end},
  {15, function() mem:write_u32(blk + 100, 2) end},          -- multiball (spoken: "Multi ball!")
  {16, function() K.A:set_value(1) end}, {16.3, function() K.A:clear_value() end},
  {17, function() K.D:set_value(1) end}, {17.3, function() K.D:clear_value() end},
  {22, function() screen:snapshot("se_multiball.png"); status("multiball") end},
  {30, function() screen:snapshot("se_multiball2.png"); status("multiball")
    local ok = true
    for _, r in ipairs(res) do
      if r.fps < 30 and r.tag ~= "multiball" then ok = false end
      if r.tps < 55 then ok = false end
    end
    if res[#res].speech == 1 and res[#res].spoken < 1 then ok = false end
    print("RESULT " .. (ok and "PASS" or "FAIL"))
    manager.machine:exit() end},
}
local done = {}
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not blk then
    if t >= next_scan then
      next_scan = t + 3
      for a = 0x400000 - 4, 0x1000, -2 do
        if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then blk = a; break end
      end
      if blk then t0 = t; print(string.format("block at %06x t=%.1f", blk, t)) end
    end
    if t > 150 then print("RESULT FAIL: no debug block"); manager.machine:exit() end
    return
  end
  for i, e in ipairs(ev) do if not done[i] and t - t0 >= e[1] then done[i] = true; e[2]() end end
end)

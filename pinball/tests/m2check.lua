-- Neural Pinball M2 check: attract mode, a player's game (Return, plunger with S, flips), the
-- three balls drained with the plunger pulled again for each new ball, a high score (test hook),
-- initials typed, back to attract mode with the new best score. Screenshots in $RUN_DIR/shots/.
--   emu/run_test.sh pinball/build/game/pinball.img pinball/tests/m2check.lua 300
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
local K = {Return = find_field({"Return"}), S = find_char("s"), A = find_char("a"), D = find_char("d"),
           E = find_char("e"), V = find_char("v")}
local blk, t0, next_scan = nil, nil, 10
local function rd(o) return mem:read_i32(blk + o) end
local function rd16(o) return mem:read_i16(blk + o) end
local function status(tag)
  print(string.format("[%s] t=%.1f mode %d gstate %d ball %d score %d inplay %d ai %d fps %.1f ticks/s %.1f best %s %d",
    tag, manager.machine.time:as_double(), rd(12), rd16(30), rd16(28), rd(24), rd16(34), rd16(32), rd(36) / 100, rd(40) / 100,
    string.char(mem:read_u8(blk + 140), mem:read_u8(blk + 141), mem:read_u8(blk + 142)), rd(144)))
end
local held = {}
local function down(k) if K[k] then K[k]:set_value(1) end end
local function up(k) if K[k] then K[k]:clear_value() end end
local phase, phase_t, shots, typed = "attract", 0, 0, 0
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not blk then
    if t >= next_scan then
      next_scan = t + 3
      for a = 0x100000 - 4, 0x1000, -2 do
        if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then blk = a; break end
      end
      if blk then t0 = t; phase_t = t; print(string.format("block at %06x t=%.1f", blk, t)) end
    end
    return
  end
  local dt = t - phase_t
  if phase == "attract" then
    if dt > 6 then screen:snapshot("1_attract.png"); status("attract"); down("Return"); phase = "start"; phase_t = t end
  elseif phase == "start" then
    if dt > 0.2 then up("Return") end
    if dt > 1.5 and dt < 2.4 then down("S") elseif dt >= 2.4 then up("S") end
    if dt > 2.9 then screen:snapshot("2_launch.png"); status("launched"); phase = "play"; phase_t = t end
  elseif phase == "play" then
    -- a few flips in the first 12 s, then hands off; S pulled again every 5 s (new ball / ball saved)
    local p = dt % 1.3
    if dt < 12 then
      if p < 0.2 then down("A") else up("A") end
      if p > 0.6 and p < 0.8 then down("D") else up("D") end
    else up("A"); up("D") end
    local q = dt % 5
    if q > 2 and q < 2.8 then down("S") else up("S") end
    if dt > 8 and shots == 0 then screen:snapshot("3_play.png"); status("play"); shots = 1 end
    if rd(12) == 3 or rd(12) == 5 or (rd16(30) == 2) then
      status("game over")
      mem:write_u32(blk + 100, 0)
      phase = "over"; phase_t = t
    end
    if rd(12) == 2 and rd16(28) == 3 and shots == 1 then
      mem:write_u32(blk + 100, 3)   -- test hook: +3,000,000 on the last ball, for a high score
      shots = 2
    end
    if dt > 240 then print("RESULT FAIL: the game did not end"); manager.machine:exit() end
  elseif phase == "over" then
    if dt > 4 and typed == 0 then screen:snapshot("4_initials.png"); status("initials") end
    local letters = {"D", "E", "V", "Return"}
    for i, k in ipairs(letters) do
      local at = 4 + i * 0.6
      if dt > at and typed == i - 1 then down(k); typed = i end
      if dt > at + 0.2 then up(k) end
    end
    if dt > 8 and shots < 3 then screen:snapshot("5_scores.png"); status("scores"); shots = 3 end
    if dt > 16 then
      screen:snapshot("6_attract_again.png")
      status("attract again")
      local best = string.char(mem:read_u8(blk + 140), mem:read_u8(blk + 141), mem:read_u8(blk + 142))
      local ok = rd(12) == 1 and best == "DEV" and rd(144) >= 3000000
      print("RESULT " .. (ok and "PASS" or "FAIL") .. string.format(" (mode %d, best %s %d)", rd(12), best, rd(144)))
      manager.machine:exit()
    end
  end
end)

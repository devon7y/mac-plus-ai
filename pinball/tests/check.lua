-- Neural Pinball MAME check: attract mode (the AI plays), then a human game (Return, pull the
-- plunger with S, a few flips). Prints frame rate (fps), game ticks per second (60.15 = physics
-- in real time) and the score; screenshots in $RUN_DIR/shots/.
--   SLOW=1.5 emu/run_test.sh pinball/build/game/pinball.img pinball/tests/check.lua 120
-- Knobs: CHECK_AT (s, when to start looking for the app; default 10), MIN_FPS (default 30).
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local screen = manager.machine.screens[":screen"]
local ports = manager.machine.ioport.ports
local MAGIC = 0x4E50424C
local CHECK_AT = tonumber(os.getenv("CHECK_AT") or "10")
local MIN_FPS = tonumber(os.getenv("MIN_FPS") or "30")

local function find_field(names)
  for _, want in ipairs(names) do
    for _, port in pairs(ports) do
      for name, f in pairs(port.fields) do if name == want then return f end end
    end
  end
end
local function find_char(c)
  local pat = "^" .. c:lower():gsub("%p", "%%%0") .. "  "
  for _, port in pairs(ports) do
    for name, f in pairs(port.fields) do if name:find(pat) then return f end end
  end
end
local K = {Return = find_field({"Return"}), S = find_char("s"), A = find_char("a"), D = find_char("d"),
           W = find_char("w"), Space = find_field({"Space"})}
for k, v in pairs(K) do if not v then print("warning: no key " .. k) end end

local blk, t0, next_scan = nil, nil, CHECK_AT
local function scan()
  local top = mem:read_u32(0x108)
  if top > 0x400000 or top < 0x20000 then top = 0x100000 end
  for a = top - 4, 0x1000, -2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then
      return a
    end
  end
end
local function rd(o) return mem:read_i32(blk + o) end
local function rd16(o) return mem:read_i16(blk + o) end

local events, done = {}, {}
local function at(dt, name, fn) events[#events + 1] = {dt, name, fn} end
local results = {}
local function report(tag)
  local fps, tps = rd(36) / 100, rd(40) / 100
  print(string.format("[%s] t=%.1f mode %d fps %.1f ticks/s %.1f score %d ball %d inplay %d ai %d late %d frames %d free %d balls (%d,%d)",
    tag, manager.machine.time:as_double(), rd(12), fps, tps, rd(24), rd16(28), rd16(34), rd16(32), rd(72), rd(16), rd(64),
    rd16(44), rd16(52)))
  results[#results + 1] = {tag = tag, fps = fps, tps = tps, score = rd(24), mode = rd(12)}
end
local function press(f, secs) if f then f:set_value(1); return secs end end

at(6, "attract shot", function() screen:snapshot("attract_1.png"); report("attract") end)
at(14, "attract shot 2", function() screen:snapshot("attract_2.png"); report("attract") end)
at(16, "Return down", function() if K.Return then K.Return:set_value(1) end end)
at(16.2, "Return up", function() if K.Return then K.Return:clear_value() end end)
at(18, "plunger pull", function() screen:snapshot("start.png"); if K.S then K.S:set_value(1) end end)
at(18.9, "plunger release", function() if K.S then K.S:clear_value() end end)
local flips = {20, 21.3, 22.1, 23.4, 24.6, 25.2, 26.8, 27.5, 28.9, 30.1, 31.4, 32.2, 33.6, 34.4}
for i, t in ipairs(flips) do
  local f = (i % 2 == 1) and "A" or "D"
  at(t, "flip " .. f, function() if K[f] then K[f]:set_value(1) end end)
  at(t + 0.25, "unflip " .. f, function() if K[f] then K[f]:clear_value() end end)
end
at(19.4, "launched shot", function() screen:snapshot("launch.png") end)
at(22, "play shot 1", function() screen:snapshot("play_1.png"); report("play") end)
at(28, "play shot 2", function() screen:snapshot("play_2.png"); report("play") end)
at(34, "play shot 3", function() screen:snapshot("play_3.png"); report("play") end)
at(35, "multiball", function() mem:write_u32(blk + 100, 2) end)
at(42, "multiball shot", function() screen:snapshot("multiball.png"); report("multiball") end)
at(48, "multiball 2", function() screen:snapshot("multiball_2.png"); report("multiball") end)
at(49, "verdict", function()
  local ok, why = true, {}
  -- (the first measurement window starts during boot; multiball's frame rate is reported, not
  -- judged, and its first window includes the pause while MacinTalk speaks the callout)
  for i, r in ipairs(results) do
    if i > 1 and r.tag ~= "multiball" then
      if r.fps < MIN_FPS then ok = false; why[#why + 1] = string.format("%s fps %.1f < %d", r.tag, r.fps, MIN_FPS) end
      if r.tps < 59 then ok = false; why[#why + 1] = string.format("%s physics %.1f ticks/s < 59", r.tag, r.tps) end
    end
  end
  local last = results[#results]
  if last.mode ~= 2 and last.mode ~= 3 and last.mode ~= 1 then ok = false; why[#why + 1] = "not in a player's game (mode " .. last.mode .. ")" end
  if last.score <= 0 then ok = false; why[#why + 1] = "score 0" end
  print("RESULT " .. (ok and "PASS" or "FAIL") .. (ok and "" or (": " .. table.concat(why, "; "))))
  manager.machine:exit()
end)

sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not blk then
    if t >= next_scan then
      blk = scan()
      next_scan = t + 3
      if blk then
        t0 = t
        print(string.format("block at %06x, t=%.1f, screen x %d", blk, t, rd(92)))
      end
    end
    if t > CHECK_AT + 90 then print("RESULT FAIL: no debug block"); manager.machine:exit() end
    return
  end
  for i, e in ipairs(events) do
    if not done[i] and t - t0 >= e[1] then done[i] = true; e[3]() end
  end
end)

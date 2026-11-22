-- Tank Duel in MAME: title / attract demo, then a game played by the autopilot (the player tank
-- is driven by the scripted hunter bot), with frame rates, screenshots and the data for the
-- bit-exactness checks (attract demo state hashes, one policy decision's inputs and logits).
--   emu/run_test.sh tanks/build/tankduel.img tanks/tests/game.lua 120
--   SLOW=1.5 emu/run_test.sh ...      (real Plus speed)
-- Environment: STAGE (stage index to start at, 0-based), START (emulated s to start the game, default 45), PLAY (s of play, default 40),
--   AIVIEW=1 (turn "Show the AI" on 10 s into the game), BENCH=1 (run the built-in benchmark on the
--   title first), SHOTS (comma-separated emulated seconds for extra screenshots).
-- Prints DBG lines (HASH k value, PROBE tick pol obs.. | logits..), FPS lines and RESULT.
mem = manager.machine.devices[":maincpu"].spaces["program"]
screen = manager.machine.screens[":screen"]
local function ram_top()
  local n, u = manager.options.entries["ramsize"]:value():match("(%d+)(%a?)")
  return tonumber(n) * ((u == "M" or u == "m") and 1048576 or (u == "K" or u == "k") and 1024 or 1)
end
local MAGIC = 0x544E4B44
local START = tonumber(os.getenv("START") or "45")
local PLAY = tonumber(os.getenv("PLAY") or "40")
local AIVIEW = os.getenv("AIVIEW") == "1"
local BENCH = os.getenv("BENCH") == "1"
local extra = {}
for s in (os.getenv("SHOTS") or ""):gmatch("[^,]+") do extra[#extra + 1] = tonumber(s) end
local blk
local function scan()
  for a = ram_top() - 8, 0x1000, -2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then
      return a
    end
  end
end
local function L(o) return mem:read_i32(blk + o) end
local function U(o) return mem:read_u32(blk + o) end
local function status(tag)
  print(string.format("FPS %s t=%.1f state=%d frames=%d simticks=%d fps=%.2f tps=%.2f stage=%d score=%d lives=%d drops=%d decisions=%d aiview=%d free=%d",
    tag, manager.machine.time:as_double(), L(12), L(16), L(20), L(24) / 100, L(28) / 100, L(32), L(36), L(40),
    L(372), L(324), L(352), L(44)))
end
local steps, done = {}, {}
local function at(t, f) steps[#steps + 1] = {t, f} end
for t = 10, START - 2, 2 do
  at(t, function()
    if not blk then
      blk = scan()
      if blk then print(string.format("DBG block at %06x (t=%.0f)", blk, manager.machine.time:as_double())) end
    end
  end)
end
at(20, function() if blk then status("title"); screen:snapshot("title.png") end end)
if BENCH then
  at(START - 25, function() if blk then mem:write_u32(blk + 328, 3) end end)
  at(START - 2, function()
    if blk then
      print(string.format("BENCH2 x300: obs %d, policy %d, 8 rays %d, los %d, aim %d, target %d (ticks)",
        L(388), L(392), L(396), L(400), L(404), L(408)))
      print(string.format("BENCH obs+policy x300 %d ticks; sim tick x300 %d ticks; frame x300 %d ticks; AI-view frame x100 %d ticks",
        L(332), L(336), L(340), L(344)))
    end
  end)
end
at(START - 1, function()
  if not blk then return end
  status("demo")
  local n = L(48)
  for k = 0, n - 1 do print(string.format("DBG HASH %d %u", k, U(52 + 4 * k))) end
  local o = {}
  for k = 0, 39 do o[#o + 1] = tostring(mem:read_i16(blk + 180 + 2 * k)) end
  local g = {}
  for k = 0, 14 do g[#g + 1] = tostring(L(260 + 4 * k)) end
  print(string.format("DBG PROBE %d %d %s | %s", L(320), L(376), table.concat(o, " "), table.concat(g, " ")))
  screen:snapshot("demo.png")
end)
at(START, function()
  if blk then
    mem:write_u32(blk + 384, tonumber(os.getenv("STAGE") or "0"))
    mem:write_u32(blk + 328, 4)
  end
end)
at(START + 6, function() if blk then status("play6"); screen:snapshot("play1.png") end end)
if AIVIEW then
  at(START + 10, function() if blk then mem:write_u32(blk + 328, 2) end end)
  at(START + 16, function() if blk then status("aiview"); screen:snapshot("aiview.png") end end)
  at(START + 20, function() if blk then mem:write_u32(blk + 328, 2) end end)
end
at(START + PLAY / 2, function() if blk then status("play_mid"); screen:snapshot("play2.png") end end)
for _, t in ipairs(extra) do at(t, function() if blk then status("shot" .. t); screen:snapshot("t" .. t .. ".png") end end) end
at(START + PLAY, function()
  if blk then status("end"); screen:snapshot("play3.png") end
  print(blk and (L(12) >= 2 and "RESULT PASS" or "RESULT FAIL (game did not start)") or "RESULT FAIL (no block)")
  manager.machine:exit()
end)
table.sort(steps, function(a, b) return a[1] < b[1] end)
local i = 1
frame_sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  while i <= #steps and t >= steps[i][1] do steps[i][2](); i = i + 1 end
end)

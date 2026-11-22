-- M2 check: the game boots, a round starts, mouse-drawn strokes get live guesses.
-- TARGET=<category index> forces round 1's word (default 0). SHOTS in $RUN_DIR/shots.
dofile(os.getenv("GUESS_TESTS") .. "/glib.lua")
local target = tonumber(os.getenv("TARGET") or "0")
run(function()
  sleep(tonumber(os.getenv("CHECK_AT") or "12"))
  if not find_block(150) then print("no debug block"); finish(false); return end
  print(string.format("block at %06x", B))
  if not wait_state(1, 120) then print("no title, state " .. u32(12) .. " modelErr " .. i32(56)); shot("err"); finish(false); return end
  print(string.format("title at t=%.1f s: load %d ticks, %d classes, code %d B, FreeMem %d B, speech %d", now(), i32(80), i32(112), u32(116), u32(52), i32(60)))
  shot("1_title")
  mailbox(1, target)
  key("\r")
  if not wait_state(2, 10) then finish(false); return end
  sleep(0.5); shot("2_card")
  key("\r")
  if not wait_state(3, 10) then finish(false); return end
  print("round 1 target " .. i32(20))
  local strokes = loadfile(os.getenv("GUESS_TESTS") .. "/" .. (os.getenv("DRAWING") or "house") .. ".lua")()
  local n0 = u32(24)
  for i, s in ipairs(strokes) do
    draw(s, 0.03)
    if u32(12) ~= 3 then break end
    print(string.format("  after stroke %d (t=%.1f): guesses %d, top %d (%d%%), last guess %d ticks, points %d",
      i, now(), u32(24), i32(28), i32(32), u32(48), i32(76)))
  end
  sleep(2.5)
  shot("3_drawing")
  sleep(3)
  print(string.format("last guess took %d ticks; spoken %d (err %d)", u32(48), i32(136), i32(140)))
  print(string.format("guesses %d, top3 %d/%d/%d (%d%%/%d%%/%d%%), state %d, score %d",
    u32(24), i32(88), i32(92), i32(96), i32(100), i32(104), i32(108), u32(12), i32(36)))
  local ok = u32(24) > n0 and i32(28) >= 0
  if u32(12) == 4 then sleep(0.5); shot("4_result") end
  if os.getenv("FULL") then             -- rounds 2..6: one stroke each, then Skip; the end screen
    local c = canvas()
    for r = 2, 6 do
      if not wait_state(2, 15) then print("no card for round " .. r); finish(false); return end
      key("\r")
      if not wait_state(3, 10) then finish(false); return end
      draw(ring(128, 128, 70 + 10 * r, 16), 0.03)
      sleep(1.5)
      mouse_to(c.left + 352, 303); sleep(0.1); btn:set_value(1); sleep(0.1); btn:clear_value(); sleep(0.3)
      if not wait_state(4, 10) then finish(false); return end
      print(string.format("round %d: target %d, top %d (%d%%), guesses %d", r, i32(20), i32(28), i32(32), u32(24)))
      key("\r")
    end
    if not wait_state(5, 15) then print("no end screen"); finish(false); return end
    sleep(0.3); shot("5_end_a"); sleep(3); shot("5_end")
    print(string.format("end: score %d, okMask %d; end screen ticks: header %d, round 0 strokes %d (n=%d), total %d", i32(36), i32(84), u32(120), u32(124), u32(128), u32(132)))
  end
  finish(ok)
end)

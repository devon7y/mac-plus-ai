-- Broadside in MAME: a whole game on autopilot (mailbox command 6 picks my shots), with the
-- options given, "Show the Mac's Thinking" on; screenshots during and at the end.
--   emu/run_test.sh broadside/build/release/broadside.img broadside/tests/game.lua 900
-- Environment: CHECK_AT (25; 70 at 4 MB), LEVEL (2), RULES (bits: 1 no touching, 2 salvo; 3),
--   MID (my shot count for the mid-game screenshot, 12)
mem = manager.machine.devices[":maincpu"].spaces["program"]
screen = manager.machine.screens[":screen"]
local function ram_top()
  local n, u = manager.options.entries["ramsize"]:value():match("(%d+)(%a?)")
  return tonumber(n) * ((u == "M" or u == "m") and 1048576 or (u == "K" or u == "k") and 1024 or 1)
end
local MAGIC = 0x42524453
local CHECK_AT = tonumber(os.getenv("CHECK_AT") or "25")
local LEVEL = tonumber(os.getenv("LEVEL") or "2")
local RULES = tonumber(os.getenv("RULES") or "3")
local MID = tonumber(os.getenv("MID") or "12")
local blk
local function scan()
  for a = ram_top() - 8, 0x1000, -2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then return a end
  end
end
local function f(o) return mem:read_u32(blk + o) end
local function cmd(c, a0, a1, a2)
  mem:write_u32(blk + 24, a0 or 0); mem:write_u32(blk + 28, a1 or 0); mem:write_u32(blk + 32, a2 or 0)
  mem:write_u32(blk + 20, c)
end
local state, nextT, midShot, t0, done0 = "scan", CHECK_AT, false, 0, 0
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if t < nextT then return end
  if state == "scan" then
    blk = scan()
    if blk and f(12) >= 1 then
      print(string.format("found BRDS at %06x, t=%.1f, freeMem=%d, speech=%d", blk, t, f(80), f(84)))
      screen:snapshot("start.png")
      done0 = f(36); cmd(1, 4242, LEVEL, RULES); state = "think"; nextT = t + 1
    else
      nextT = t + 2
      if t > CHECK_AT + 120 then print("RESULT FAIL (block not found)"); manager.machine:exit() end
    end
  elseif state == "think" then
    if f(36) == done0 then nextT = t + 0.5; return end     -- (wait until the new game is set up)
    done0 = f(36); cmd(3, 1); state = "play"; nextT = t + 3; t0 = t
  elseif state == "play" then
    if f(52) ~= 0 then
      nextT = t + 3; state = "over"
    elseif f(12) == 2 and f(20) == 0 then
      if not midShot and f(44) >= MID then
        midShot = true; nextT = t + 4; return     -- let the heat map settle, then the screenshot
      end
      if midShot == true then
        screen:snapshot("mid.png"); midShot = "done"
        print(string.format("mid: think %d, draw_all calls %d, Mac samples %d", f(88 + 30 * 4), f(88 + 29 * 4), f(72)))
      end
      cmd(6)
      nextT = t + 1
    else
      nextT = t + 0.5
    end
    if t - t0 > 1500 then print("RESULT FAIL (game did not end)"); manager.machine:exit() end
  elseif state == "over" then
    screen:snapshot("end.png")
    print(string.format("end: think %d, draw_all calls %d", f(88 + 30 * 4), f(88 + 29 * 4)))
    print(string.format("game over at t=%.0f: winner %s, my shots %d, Mac shots %d, Mac moves %d, AI ticks max %d mean %.1f",
      t, f(52) == 1 and "me" or "the Mac", f(44), f(48), f(64), f(60), f(68) / math.max(1, f(64))))
    print("RESULT PASS")
    manager.machine:exit()
  end
end)

-- Orbit Duel M0 check: title screen, then a game against the computer with the autopilot flying
-- our ship (computer vs computer), frame rate over 20 s of emulated time, screenshots.
--   SLOW=1.5 emu/run_test.sh duel/build/orbitduel.img duel/tests/m0.lua 90
-- Env: CHECK_AT (s, default 12), PLAY_S (s of play to measure, default 20), KIND (1 solo, 2 hotseat)
mem = manager.machine.devices[":maincpu"].spaces["program"]
screen = manager.machine.screens[":screen"]
local MAGIC = 0x4F44554C
local blk, t_start, f_start, phase = nil, nil, nil, "find"
local check_at = tonumber(os.getenv("CHECK_AT") or "12")
local play_s = tonumber(os.getenv("PLAY_S") or "20")
local kind = tonumber(os.getenv("KIND") or "1")
local shots = 0

local function scan()
  local top = mem:read_u32(0x108)
  if top < 0x80000 or top > 0x400000 then top = 0x100000 end
  for a = 0x1000, top - 4, 2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then
      return a
    end
  end
end
local function rd(o) return mem:read_i32(blk + o) end
local function rd16(o) return mem:read_i16(blk + o) end
local function wr(o, v) mem:write_u32(blk + o, v) end

sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if phase == "find" then
    if t >= check_at then
      blk = scan()
      if blk then
        print(string.format("block at %06x t=%.1f screen=%d bounds=(%d,%d,%d,%d) view=(%d,%d) freemem=%d",
          blk, t, rd(12), rd16(72), rd16(74), rd16(76), rd16(78), rd16(80), rd16(82), rd(68)))
        phase, t_start = "title", t
      else
        check_at = t + 3
      end
    end
    if t > 120 then print("RESULT FAIL (no debug block)"); manager.machine:exit() end
  elseif phase == "title" then
    if t > t_start + 3 then
      screen:snapshot("title.png")
      wr(52, 1)                         -- autopilot: the computer flies our ship too
      wr(48, 1)                         -- level normal
      wr(44, kind)                      -- start the game
      phase, t_start = "start", t
    end
  elseif phase == "start" then
    if rd(12) == 2 and t > t_start + 2 then
      phase, t_start, f_start = "play", t, rd(20)
      tk_start = mem:read_u32(0x16A)
      print(string.format("game started (kind %d) at t=%.1f", rd(16), t))
    elseif t > t_start + 10 then print("RESULT FAIL (game did not start, screen " .. rd(12) .. ")"); manager.machine:exit() end
  elseif phase == "play" then
    local dt = t - t_start
    if shots < 3 and dt > 4 + shots * 6 then
      shots = shots + 1
      screen:snapshot(string.format("game_%d.png", shots))
      print(string.format("shot %d t=%.1f tick=%d score %d:%d phase %d round %d", shots, t, rd(28), rd16(32), rd16(34), rd16(36), rd16(38)))
    end
    if dt >= play_s then
      local frames, ticks = rd(20) - f_start, mem:read_u32(0x16A) - tk_start
      local fps = frames / (ticks / 60.15)
      print(string.format("frames %d in %d ticks (%.1f s Mac time): %.1f fps; sim tick %d; work %.1f ms/frame",
        frames, ticks, ticks / 60.15, fps, rd(28), rd(24) / math.max(rd(20), 1) * 16.63))
      print(string.format("score %d:%d round %d matches %d", rd16(32), rd16(34), rd16(38), rd(40)))
      local ok = fps >= 27 and rd(28) > 0
      print(ok and string.format("RESULT PASS fps=%.1f", fps) or string.format("RESULT FAIL fps=%.1f", fps))
      manager.machine:exit()
    end
  end
end)

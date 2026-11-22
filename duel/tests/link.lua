-- Orbit Duel link test, one side: boot, start the serial cable game (MAME test speed) or the
-- AppleTalk screen, let the computer fly our ship, play for PLAY_S seconds of the game, then
-- (when the simulation reaches step PLAY_S x 30) print the lockstep numbers (state checksums at steps 30..240, compares, waits).
--   run by duel/tests/serial_link.sh (two instances); SIDE=A|B names the log lines.
-- Env: LINK (4 serial, 3 AppleTalk), PLAY_S (default 60), CHECK_AT (default 12)
mem = manager.machine.devices[":maincpu"].spaces["program"]
screen = manager.machine.screens[":screen"]
local MAGIC = 0x4F44554C
local side = os.getenv("SIDE") or "?"
local link = tonumber(os.getenv("LINK") or "4")
local play_s = tonumber(os.getenv("PLAY_S") or "60")
local check_at = tonumber(os.getenv("CHECK_AT") or "12")
local blk, phase, t_phase, shots, done_at = nil, "find", 0, 0, nil

-- the null modem's speed: index 21 = 7200 baud, which is what MAME's fast SCC clock makes of the
-- app's test speed (serial.c); the real game uses 19,200
for tag, port in pairs(manager.machine.ioport.ports) do
  if tag:find("null_modem") then
    for fname, f in pairs(port.fields) do
      if fname == "TX Baud" or fname == "RX Baud" then f.user_value = 21 end
    end
  end
end

local function scan()
  for a = 0x1000, 0x3FFFF0, 2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then return a end
  end
end
local function rd(o) return mem:read_i32(blk + o) end
local function rdu(o) return mem:read_u32(blk + o) end
local function rd16(o) return mem:read_i16(blk + o) end
local function wr(o, v) mem:write_u32(blk + o, v) end
local function log(fmt, ...) print(string.format("[%s] " .. fmt, side, ...)) end

sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if phase == "find" then
    if t >= check_at then
      blk = scan()
      if blk and rd(12) == 1 then phase, t_phase = "title", t else check_at = t + 2 end
    end
    if t > 150 then log("RESULT FAIL (no title)"); manager.machine:exit() end
  elseif phase == "title" and t > t_phase + 2 then
    wr(52, 1)                                   -- autopilot
    wr(48, 77)                                  -- arg 77: the MAME test speed
    wr(44, link)
    phase, t_phase = "link", t
    log("link screen requested at t=%.1f", t)
  elseif phase == "link" then
    if rd(12) == 2 then
      phase, t_phase = "play", t
      log("game started at t=%.1f role=%d seed=%08x delay=%d", t, rd(92), rdu(180), rd(184))
    elseif t > t_phase + 120 then
      screen:snapshot("link_timeout.png")
      log("RESULT FAIL (no game: link state %d, rx %d tx %d bad %d err %d)", rd(88), rd(112), rd(116), rd(120), rd(128))
      manager.machine:exit()
    end
  elseif phase == "play" then
    local dt = t - t_phase
    if shots < 2 and dt > 10 + shots * 25 then
      shots = shots + 1
      screen:snapshot(string.format("%s_game_%d.png", side, shots))
    end
    if done_at then
      if t > done_at + 5 then manager.machine:exit() end   -- let the other Mac finish its last steps
      return
    end
    if rd(28) >= play_s * 30 or rd(12) ~= 2 or dt > play_s * 2 then
      local h = {}
      for k = 0, 7 do h[#h + 1] = string.format("%08x", rdu(196 + 4 * k)) end
      log("tick %d remote %d score %d:%d round %d", rd(28), rd(108), rd16(32), rd16(34), rd16(38))
      log("checksums compared ok %d bad %d; packets rx %d tx %d bad %d; waits %d frames %d",
        rd(96), rd(100), rd(112), rd(116), rd(120), rd(124), rd(20))
      log("SUMS %s", table.concat(h, " "))
      log("RESULT %s", (rd(100) == 0 and rd(96) >= 10 and rd(12) == 2) and "PASS" or "FAIL")
      done_at = t
    end
  end
end)

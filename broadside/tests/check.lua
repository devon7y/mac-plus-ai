-- Broadside in MAME: finds the app's 'BRDS' debug block, optionally runs the AI benchmark
-- (the same games as host/sim.c, so the shots must match the host), then plays a few turns of
-- a Hard game with "Show the Mac's Thinking" on, clicking the Mac's grid with the real mouse.
--   emu/run_test.sh broadside/build/test/broadside_test.img broadside/tests/check.lua 400
-- Environment:
--   CHECK_AT  emulated second to start looking (default 25; 70 at 4 MB)
--   BENCH     number of benchmark games (0 = skip; default 0); BENCH_LEVEL (2), BENCH_SAMPLES (0 = default)
--   TURNS     turns to play (default 4)
--   EXPECT_W  screen width (448 with Narrow Screen, 512 without; default 448)
mem = manager.machine.devices[":maincpu"].spaces["program"]
screen = manager.machine.screens[":screen"]
local ports = manager.machine.ioport.ports
btn = (ports[":macadb:MOUSE0"] and ports[":macadb:MOUSE0"].fields["Mouse Button 0"]) or ports[":MOUSE0"].fields["Mouse Button"]
local function mouse_to(h, v)
  mem:write_u16(0x828, v); mem:write_u16(0x82A, h)
  mem:write_u16(0x82C, v); mem:write_u16(0x82E, h)
  mem:write_u8(0x8CE, mem:read_u8(0x8CF))
end
local function ram_top()
  local n, u = manager.options.entries["ramsize"]:value():match("(%d+)(%a?)")
  return tonumber(n) * ((u == "M" or u == "m") and 1048576 or (u == "K" or u == "k") and 1024 or 1)
end
local MAGIC = 0x42524453
local CHECK_AT = tonumber(os.getenv("CHECK_AT") or "25")
local BENCH = tonumber(os.getenv("BENCH") or "0")
local BENCH_LEVEL = tonumber(os.getenv("BENCH_LEVEL") or "2")
local BENCH_SAMPLES = tonumber(os.getenv("BENCH_SAMPLES") or "0")
local TURNS = tonumber(os.getenv("TURNS") or "4")
local EXPECT_W = tonumber(os.getenv("EXPECT_W") or "448")
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
-- the Mac's grid (grid 1) cell centre, from the layout in board.c
local CS = EXPECT_W >= 470 and 20 or 18
local gap = EXPECT_W >= 470 and 30 or 22
local gw = 10 * CS + 1
local x0 = (EXPECT_W - (2 * (12 + gw) + gap)) // 2
local g1l = x0 + 2 * 12 + gw + gap
local function cell_xy(c) return g1l + (c % 10) * CS + CS // 2 + 1, 50 + (c // 10) * CS + CS // 2 + 1 end

local state, nextT, done0 = "scan", CHECK_AT, 0
local shots = {11, 44, 47, 74, 22, 66, 88, 33}
local turn = 0
local heat_shot = false
local fails = 0
local t_bench
local cpu = manager.machine.devices[":maincpu"]
local PCS = os.getenv("PCS")       -- PC histogram during the benchmark
local hist = {}
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if PCS and state == "bench" then
    local pc = cpu.state["PC"].value
    hist[pc] = (hist[pc] or 0) + 1
  end
  if t < nextT then return end
  if state == "scan" then
    blk = scan()
    if blk and f(12) >= 1 then
      print(string.format("found BRDS at %06x, t=%.1f, state=%d, freeMem=%d, speech=%d", blk, t, f(12), f(80), f(84)))
      screen:snapshot("place.png")
      if BENCH > 0 then
        done0 = f(36); cmd(4, BENCH, BENCH_LEVEL, BENCH_SAMPLES); t_bench = t; state = "bench"
      else
        state = "newgame"
      end
    else
      nextT = t + 2
      if t > CHECK_AT + 120 then print("RESULT FAIL (block not found)"); manager.machine:exit() end
    end
  elseif state == "bench" then
    if f(36) ~= done0 then
      local line = "bench shots:"
      for i = 0, f(88 + 28 * 4) - 1 do line = line .. " " .. f(88 + i * 4) end
      local moves, ticks, mx, samp = f(88 + 24 * 4), f(88 + 25 * 4), f(88 + 26 * 4), f(88 + 27 * 4)
      print(line)
      if PCS then
        local anchor = f(216)
        local a = {}
        for k, v in pairs(hist) do a[#a + 1] = {k, v} end
        table.sort(a, function(x, y) return x[2] > y[2] end)
        local fh = io.open(os.getenv("RUN_DIR") .. "/pcs.txt", "w")
        fh:write(string.format("anchor %08X\n", anchor))
        for i = 1, #a do fh:write(string.format("%08X %d\n", a[i][1], a[i][2])) end
        fh:close()
      end
      print(string.format("bench: %d moves, %d ticks (%.3f s/move), max %d ticks (%.2f s), %.0f samples/move, %.1f s emulated",
        moves, ticks, ticks / moves / 60.15, mx, mx / 60.15, samp / moves, t - t_bench))
      state = "newgame"
    end
    nextT = t + 1
  elseif state == "newgame" then
    done0 = f(36); cmd(1, 777, 2, 0); state = "think"; nextT = t + 1
  elseif state == "think" then
    if f(36) == done0 then nextT = t + 0.5; return end
    cmd(3, 1); state = "fire"; nextT = t + 3
  elseif state == "fire" then
    -- my turn again (and my last click was taken)?
    if f(12) == 2 and f(44) >= turn then
      if turn == 2 and not heat_shot then
        heat_shot = true; nextT = t + 6; return      -- let the Mac think during my turn
      end
      if turn == 2 then screen:snapshot("thinking.png") end
      turn = turn + 1
      if turn > TURNS then
        screen:snapshot("battle.png")
        print(string.format("after %d turns: my shots %d, Mac shots %d, AI ticks last %d max %d (moves %d), samples %d exact %d",
          TURNS, f(44), f(48), f(56), f(60), f(64), f(72), f(76)))
        if f(44) ~= TURNS or f(48) ~= TURNS then fails = fails + 1 end
        state = "end"
      else
        local x, y = cell_xy(shots[turn])
        mouse_to(x, y); btn:set_value(1); state = "release"; nextT = t + 0.2
      end
    else
      nextT = t + 0.5
    end
  elseif state == "release" then
    btn:clear_value(); state = "fire"; nextT = t + 1
  elseif state == "end" then
    print(fails == 0 and "RESULT PASS" or "RESULT FAIL")
    manager.machine:exit()
  end
end)

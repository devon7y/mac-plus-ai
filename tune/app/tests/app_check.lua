-- Tune in MAME: compose a tune through the test mailbox, let it play, save and copy.
--   emu/run_test.sh tune/build/tune_plus.img tune/app/tests/app_check.lua 400
-- Env: SEED (default 4711), TYPE (index, default 0 reel), KEY (index, default 1 D major),
--      CHECK_AT (s, default 15; 4 MB: 45), PLAY_FOR (s of playing to wait, default 20)
mem = manager.machine.devices[":maincpu"].spaces["program"]
local screen = manager.machine.screens[":screen"]
local function ram_top()
  local n, u = manager.options.entries["ramsize"]:value():match("(%d+)(%a?)")
  return tonumber(n) * ((u == "M" or u == "m") and 1048576 or (u == "K" or u == "k") and 1024 or 1)
end
local MAGIC = 0x544E4442
local SEED = tonumber(os.getenv("SEED") or "4711")
local TYPE = tonumber(os.getenv("TYPE") or "0")
local KEY = tonumber(os.getenv("KEY") or "1")
local CHECK_AT = tonumber(os.getenv("CHECK_AT") or "15")
local PLAY_FOR = tonumber(os.getenv("PLAY_FOR") or "20")
local blk, step, t_next, t_compose = nil, "scan", CHECK_AT, 0
local fails = 0
local function check(ok, what) print(string.format("CHECK %-4s %s", ok and "ok" or "FAIL", what)); if not ok then fails = fails + 1 end end
local function scan()
  for a = ram_top() - 8, 0x1000, -2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then return a end
  end
end
-- block offsets (main.c struct Dbg)
local O = {state = 12, cmd = 16, arg = 20, seed = 24, type = 28, key = 30, mode = 32, meter = 34, ntok = 36,
  t_start = 40, t_end = 44, pdbg = 48, nev = 72, nslot = 74, sp = 76, playing = 78, freeMem = 80, cmdDone = 84,
  cur_note = 88, play_start = 92, toks = 96, t_engine = 736, t_handle = 740}
local function cmd(c, a) mem:write_u32(blk + O.arg, a); mem:write_u32(blk + O.cmd, c) end
local function idle() return mem:read_u32(blk + O.cmd) == 0 end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if t < t_next then return end
  if step == "scan" then
    blk = blk or scan()
    if blk and mem:read_i32(blk + O.state) ~= -100 then
      print(string.format("block at %06x t=%.1f state=%d freeMem=%d", blk, t, mem:read_i32(blk + O.state), mem:read_u32(blk + O.freeMem)))
      check(mem:read_i32(blk + O.state) == 0, "model loaded (state 0)")
      screen:snapshot("start.png")
      mem:write_u32(blk + O.arg, TYPE); mem:write_u32(blk + O.cmd, 5)
      step = "key"; t_next = t + 0.5
    else t_next = t + 3; if t > CHECK_AT + 60 then print("TIMEOUT no block"); fails = fails + 1; step = "end" end end
  elseif step == "key" then if idle() then cmd(6, KEY); step = "compose" end; t_next = t + 0.5
  elseif step == "compose" then if idle() then cmd(1, SEED); t_compose = t; step = "wait"; t_next = t + 10 else t_next = t + 0.5 end
  elseif step == "wait" then
    local st = mem:read_i32(blk + O.state)
    print(string.format("t=%.0f state=%d ntok=%d nev=%d nslot=%d playing=%d", t, st, mem:read_i32(blk + O.ntok),
      mem:read_i16(blk + O.nev), mem:read_i16(blk + O.nslot), mem:read_i16(blk + O.playing)))
    if t - t_compose > 20 and t - t_compose < 31 then screen:snapshot("composing.png") end
    if st == 2 then
      local n = mem:read_i32(blk + O.ntok)
      local ticks = mem:read_u32(blk + O.t_end) - mem:read_u32(blk + O.t_start)
      print(string.format("composed %d tokens in %d ticks = %.1f s (%.3f s/token); model %.3f s/token, drawing+lists %.3f s/token",
        n, ticks, ticks / 60.15, ticks / 60.15 / (n - 1), mem:read_u32(blk + O.t_engine) / 60.15 / (n - 1),
        mem:read_u32(blk + O.t_handle) / 60.15 / (n - 1)))
      local toks = {}
      for k = 0, math.min(n, 640) - 1 do toks[#toks + 1] = tostring(mem:read_u8(blk + O.toks + k)) end
      local f = io.open(os.getenv("RUN_DIR") .. "/app_tokens.txt", "w")
      f:write(string.format("%d %d %d %d %d\n", mem:read_u32(blk + O.seed), mem:read_i16(blk + O.type),
        mem:read_i16(blk + O.key), mem:read_i16(blk + O.mode), mem:read_i16(blk + O.meter)))
      f:write(table.concat(toks, " ") .. "\n"); f:close()
      check(n > 30, "tune composed (" .. n .. " tokens)")
      screen:snapshot("composed.png")
      step = "play"; t_next = t + PLAY_FOR
    elseif t - t_compose > 600 then print("TIMEOUT composing"); fails = fails + 1; step = "end" end
    if step == "wait" then t_next = t + 10 end
  elseif step == "play" then
    local p = mem:read_i16(blk + O.playing)
    local pd = {}
    for k = 0, 5 do pd[#pd + 1] = string.format("%x", mem:read_u32(blk + O.pdbg + 4 * k)) end
    print("player " .. table.concat(pd, " ") .. " cur_note " .. mem:read_i16(blk + O.cur_note))
    check(mem:read_u32(blk + O.play_start) ~= 0, "playback started")
    screen:snapshot("playing.png")
    cmd(7, 0); step = "saved"; t_next = t + 3
  elseif step == "saved" then
    check(mem:read_u32(blk + O.cmdDone) == 0x53415645, "Save wrote Tune.abc")
    cmd(4, 0); step = "copied"; t_next = t + 3
  elseif step == "copied" then
    check(mem:read_u32(blk + O.cmdDone) == 0x434F5059, "Copy put a PICT on the clipboard")
    screen:snapshot("after_copy.png")
    cmd(8, 0); step = "about"; t_next = t + 2
  elseif step == "about" then
    check(mem:read_u32(blk + O.cmdDone) == 0x41424F55, "About box shown")
    screen:snapshot("about.png")
    cmd(99, 0); step = "end"; t_next = t + 1
  elseif step == "end" then
    print(fails == 0 and "RESULT PASS" or ("RESULT FAIL (" .. fails .. ")"))
    manager.machine:exit()
  end
end)

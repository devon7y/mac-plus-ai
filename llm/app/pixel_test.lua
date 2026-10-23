-- MAME test for Pixel (test build with a "Pixel Test" script on the boot disk).
-- Finds the debug block 'PXL!' (2-byte steps), reports loading and generation timings, writes the
-- token log to $RUN_DIR/tokens.txt and screenshots to $RUN_DIR/shots/, then exits.
-- Env: LATE_DISK2=<image> LATE_AT=<s> insert disk 2 into the external drive at that emulated time;
--      SHOTS="t1,t2" extra screenshot times; TIMEOUT (emulated s, default 3000).
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local run_dir = os.getenv("RUN_DIR") or "."
local timeout = tonumber(os.getenv("TIMEOUT") or "3000")
local late = os.getenv("LATE_DISK2")
local late_at = tonumber(os.getenv("LATE_AT") or "0")
local shots = {}
for t in string.gmatch(os.getenv("SHOTS") or "", "[^,]+") do shots[#shots + 1] = tonumber(t) end
local MAGIC = 0x50584C21
local base, next_scan, last_state, late_done, shot_i = nil, 30, nil, false, 1
local loaded_t = nil
-- Swap test: SWAP_WRONG=<img> SWAP_DISK2=<img> (no FLOP2): when Pixel asks for disk 2, insert the wrong
-- disk into the internal drive, then (after Pixel rejects it) the real disk 2.
local swap_wrong, swap_disk2 = os.getenv("SWAP_WRONG"), os.getenv("SWAP_DISK2")
local swap_stage, swap_t = 0, 0
-- Typing test: TYPE="text" types it (natural keyboard) + Return after the test script is done (state 4)
local typed_text = os.getenv("TYPE")
local typed, type_t, return_done, ret_down = false, nil, false, nil
local ramtop = manager.machine.devices[":maincpu"].spaces["program"]
local function scan()
  for a = 0x8000, 0x3FFFF0, 2 do
    if mem:read_u16(a) == 0x5058 and mem:read_u32(a) == MAGIC and mem:read_u32(a + 8) == 0xAFA7B3DE
       and mem:read_u32(a + 4) == 1 then return a end
  end
end
local function s32(v) if v >= 0x80000000 then return v - 0x100000000 end return v end
local function f(off) return s32(mem:read_u32(base + off)) end
local function shot(name) manager.machine.video:snapshot(); print("SHOT " .. name) end
local function report(tag)
  print(string.format("%s t=%.1f state=%d variant=%d loadErr=%d loadTicks=%d readTicks=%d maxBlock=%d freeAfter=%d "
    .. "ntok=%d genTicks=%d genSteps=%d ingTicks=%d ingSteps=%d kself=%d swaps=%d turns=%d seed=%d selftest=%d",
    tag, manager.machine.time:as_double(), f(12), f(16), f(20), f(24), f(76), f(28), f(32), f(36), f(40), f(44),
    f(48), f(52), f(56), f(64), f(68), f(72), f(80)))
  print(string.format("%s minFree=%d", tag, f(188)))
end
local function report_prof()
  local names = {"norm", "tern", "att", "attk", "ffn", "head", "sample", "aq", "az", "as", "ar", "fr", "fs", "fq"}
  local t = {}
  for i = 0, 13 do t[#t + 1] = names[i + 1] .. "=" .. f(84 + 4 * i) end
  print("PROF " .. table.concat(t, " "))
  print(string.format("KBAD apply16c=%d dot8=%d scores=%d softp=%d softw=%d rshr=%d total=%d",
    f(156), f(160), f(164), f(168), f(172), f(176), f(180)))
  print(string.format("LOADPROF emb=%d terndec=%d expand=%d sort=%d", f(140), f(144), f(148), f(152)))
end
local function dump_tokens()
  local n, addr = f(36), mem:read_u32(base + 60)
  local t = {}
  for i = 0, n - 1 do
    local v = mem:read_u16(addr + 2 * i)
    if v >= 0x8000 then v = v - 0x10000 end
    t[#t + 1] = tostring(v)
  end
  local fh = io.open(run_dir .. "/tokens.txt", "w")
  fh:write(table.concat(t, " ") .. "\n")
  fh:close()
  print("TOKENS " .. table.concat(t, " "))
end
local function insert_late()
  for tag, img in pairs(manager.machine.images) do
    if string.find(tag, "flop") or string.find(tag, "floppy") then print("IMAGE DEVICE " .. tag) end
  end
  local dev = manager.machine.images[":fdc:1:35dd"] or manager.machine.images[":flop2"]
  if not dev then
    for tag, img in pairs(manager.machine.images) do
      if string.find(tag, ":1:") then dev = img end
    end
  end
  if dev then
    local ok, err = pcall(function() dev:load(late) end)
    print("LATE INSERT " .. tostring(ok) .. " " .. tostring(err) .. " into " .. dev.device.tag)
  else
    print("LATE INSERT: no second floppy device found")
  end
end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if shot_i <= #shots and t >= shots[shot_i] then shot(string.format("t%.0f", t)); shot_i = shot_i + 1 end
  if late and not late_done and t >= late_at then late_done = true; insert_late() end
  if t > timeout then
    if base then report("TIMEOUT") end
    print("RESULT TIMEOUT")
    shot("timeout")
    manager.machine:exit()
    return
  end
  if not base then
    if t >= next_scan then
      base = scan()
      next_scan = t + 10
      if base then print(string.format("block at %06x (t=%.0fs)", base, t)) end
    end
    return
  end
  if swap_disk2 then
    local swaps = f(64)
    local dev = nil
    for tag, img in pairs(manager.machine.images) do if tag:match("^:fdc:0:") then dev = img end end
    if swap_stage == 1 and not swap_wrong then swap_stage = 3 end      -- no wrong disk: go straight to disk 2
    if swap_stage == 0 and swaps >= 1 then swap_stage = 1; swap_t = t + 3; shot("ask_disk2") end
    if swap_stage == 1 and t >= swap_t then
      local ok, err = pcall(function() dev:load(swap_wrong) end)
      print(string.format("SWAP t=%.1f inserted wrong disk: %s %s", t, tostring(ok), tostring(err)))
      swap_stage = 2
    end
    if swap_stage == 2 and swaps >= 2 then swap_stage = 3; swap_t = t + 3; shot("wrong_disk") end
    if swap_stage == 3 and t >= swap_t then
      local ok, err = pcall(function() dev:load(swap_disk2) end)
      print(string.format("SWAP t=%.1f inserted disk 2: %s %s", t, tostring(ok), tostring(err)))
      swap_stage = 4
    end
  end
  local st = f(12)
  if st ~= last_state then
    last_state = st
    report("STATE")
    if st == 2 and not loaded_t then loaded_t = t; shot("ready") end
  end
  if typed_text and st == 4 and not typed then
    typed = true
    print("TYPING " .. typed_text)
    manager.machine.natkeyboard:post(typed_text)
    type_t = t + 1 + #typed_text * 0.25
    return
  end
  if typed and not return_done and type_t and t >= type_t then
    local f = manager.machine.ioport.ports[":kbd:usp:ROW4"].fields["Return"]
    if not ret_down then f:set_value(1); ret_down = t elseif t > ret_down + 0.2 then f:clear_value(); return_done = true; shot("typed") end
    return
  end
  if typed and (not return_done or f(68) < 1 or st ~= 2) then
    if t > timeout then print("RESULT TIMEOUT") manager.machine:exit() end
    return
  end
  if os.getenv("READY") and st == 2 and not typed_text then
    report("READY")
    shot("ready_screen")
    print("RESULT READY")
    manager.machine:exit()
    return
  end
  if st == 4 or st < 0 or (typed and st == 2) then
    report("DONE")
    report_prof()
    dump_tokens()
    shot("done")
    print((st == 4 or (typed and st == 2)) and "RESULT FINISHED" or "RESULT ERROR")
    manager.machine:exit()
  end
end)

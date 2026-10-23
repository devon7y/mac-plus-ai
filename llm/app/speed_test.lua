-- Reply speed and transcript check on a release build (Pixel or PixelSE) whose disk has a
-- header-only "Pixel Test" file ("seed temp topk maxnew 0", fixed seed, no lines): the app reaches
-- state 4, then runs interactively. Types each line of $CONVO ("|"-separated) with Return through
-- the keyboard, waits for the reply, and per turn prints the reply time in emulated seconds (state
-- 3 -> back to 2) next to the engine's own ticks, so the UI's share is visible:
--   TURN n dur=<s> gen=<steps>/<ticks> ing=<steps>/<ticks> sample=<ticks> ui=<ticks>
-- ui = dur * 60.15 - gen ticks - ing ticks - sample ticks (drawing, events, encoding).
-- Screenshots: SHOT turn_N_mid (MID s after the reply starts, default 25) and turn_N (after it).
-- Env: CONVO, MID, TYPEAHEAD (typed during the first reply), TIMEOUT (emulated s, default 4000).
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local MAGIC = 0x50584C21
local lines = {}
for s in string.gmatch(os.getenv("CONVO") or "hi pixel|what do you like to do?", "[^|]+") do lines[#lines + 1] = s end
local MID = tonumber(os.getenv("MID") or "25")
local TIMEOUT = tonumber(os.getenv("TIMEOUT") or "4000")
local typeahead = os.getenv("TYPEAHEAD")
local base, next_scan, n, phase, t_wait = nil, 30, 1, "wait4", 0
local turns0, t_busy, mid_done, ret = 0, nil, false, nil
local g0 = {}
local tot = {dur = 0, gs = 0, gt = 0, is = 0, it = 0, st = 0, ui = 0}
local function f(off) local v = mem:read_u32(base + off); if v >= 0x80000000 then v = v - 0x100000000 end; return v end
local function shot(name) manager.machine.video:snapshot(); print(string.format("SHOT %s t=%.1f", name, manager.machine.time:as_double())) end
local function field(names)
  for _, nm in ipairs(names) do
    for _, port in pairs(manager.machine.ioport.ports) do
      if port.fields[nm] then return port.fields[nm] end
    end
  end
end
local function snap() return {gt = f(40), gs = f(44), it = f(48), is = f(52), st = f(108)} end
local function dump_tokens()
  local cnt, addr, t = f(36), mem:read_u32(base + 60), {}
  for i = 0, cnt - 1 do local v = mem:read_u16(addr + 2 * i); if v >= 0x8000 then v = v - 0x10000 end; t[#t + 1] = v end
  local fh = io.open((os.getenv("RUN_DIR") or ".") .. "/tokens.txt", "w"); fh:write(table.concat(t, " ") .. "\n"); fh:close()
  print("TOKENS " .. table.concat(t, " "))
end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if t > TIMEOUT then print("RESULT TIMEOUT"); shot("timeout"); manager.machine:exit() return end
  if not base then
    if t >= next_scan then
      next_scan = t + 10
      for a = 0x8000, 0x3FFFF0, 2 do
        if mem:read_u16(a) == 0x5058 and mem:read_u32(a) == MAGIC and mem:read_u32(a + 8) == 0xAFA7B3DE then base = a break end
      end
      if base then print(string.format("block at %06x (t=%.0fs)", base, t)) end
    end
    return
  end
  local st = f(12)
  if phase == "wait4" then
    if st == 4 then
      print(string.format("READY t=%.1f loadTicks=%d variant=%d seed=%d freeAfter=%d", t, f(24), f(16), f(72), f(32)))
      shot("ready"); phase, t_wait = "idle", t + 2
    elseif st < 0 then print("RESULT ERROR state " .. st); shot("error"); manager.machine:exit() end
    return
  end
  if phase == "busy" then
    if not t_busy and st == 3 then
      t_busy = t
      if typeahead and n == 1 then manager.machine.natkeyboard:post(typeahead) end
    end
    if t_busy and not mid_done and t >= t_busy + MID then mid_done = true; shot(string.format("turn_%d_mid", n)) end
    if t_busy and f(68) > turns0 and st == 2 then
      local g = snap()
      local d = {dur = t - t_busy, gs = g.gs - g0.gs, gt = g.gt - g0.gt, is = g.is - g0.is, it = g.it - g0.it, st = g.st - g0.st}
      d.ui = d.dur * 60.15 - d.gt - d.it - d.st
      for k, v in pairs(d) do tot[k] = tot[k] + v end
      print(string.format("TURN %d dur=%.2f gen=%d/%d ing=%d/%d sample=%d ui=%.1f ticks (%.2f per step) gen s/tok=%.3f",
        n, d.dur, d.gs, d.gt, d.is, d.it, d.st, d.ui, d.ui / (d.gs + d.is), d.gs > 0 and d.gt / d.gs / 60.15 or 0))
      shot(string.format("turn_%d", n))
      n, phase, t_wait, t_busy, mid_done = n + 1, "idle", t + 1, nil, false
    end
    return
  end
  if t < t_wait then return end
  if phase == "idle" and (st == 2 or st == 4) then
    if n > #lines then
      print(string.format("TOTAL dur=%.2f gen=%d/%d ing=%d/%d sample=%d ui=%.1f ticks (%.3f per step); "
        .. "steps/s=%.4f gen s/tok=%.3f", tot.dur, tot.gs, tot.gt, tot.is, tot.it, tot.st, tot.ui,
        tot.ui / (tot.gs + tot.is), (tot.gs + tot.is) / tot.dur, tot.gt / tot.gs / 60.15))
      dump_tokens()
      print("RESULT CONVO DONE"); manager.machine:exit() return
    end
    manager.machine.natkeyboard:post(lines[n])
    phase, t_wait = "typed", t + 1 + #lines[n] * 0.2
  elseif phase == "typed" then
    ret = field({"Return"}); ret:set_value(1); turns0 = f(68); g0 = snap()
    phase, t_wait = "down", t + 0.2
  elseif phase == "down" then
    ret:clear_value(); phase = "busy"
  end
end)

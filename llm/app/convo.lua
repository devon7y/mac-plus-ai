-- A real conversation on the release build: types each line of $CONVO (separated by "|") with
-- Return, waits for Pixel's reply, takes a screenshot after each turn (SHOT turn_N), then exits.
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local MAGIC = 0x50584C21
local lines = {}
for s in string.gmatch(os.getenv("CONVO") or "hi pixel", "[^|]+") do lines[#lines + 1] = s end
local base, next_scan, n, phase, t_wait, turns0, ret = nil, 30, 1, "idle", 0, 0, nil
local function f(off) local v = mem:read_u32(base + off); if v >= 0x80000000 then v = v - 0x100000000 end; return v end
local function field(names)
  for _, nm in ipairs(names) do
    for _, port in pairs(manager.machine.ioport.ports) do
      if port.fields[nm] then return port.fields[nm] end
    end
  end
end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if t > 3000 then print("RESULT TIMEOUT"); manager.machine:exit() return end
  if not base then
    if t >= next_scan then
      next_scan = t + 10
      for a = 0x8000, 0x3FFFF0, 2 do
        if mem:read_u16(a) == 0x5058 and mem:read_u32(a) == MAGIC and mem:read_u32(a + 8) == 0xAFA7B3DE then base = a break end
      end
    end
    return
  end
  if t < t_wait then return end
  if phase == "idle" and f(12) == 2 then
    if n > #lines then print("RESULT CONVO DONE"); manager.machine:exit() return end
    manager.machine.natkeyboard:post(lines[n])
    phase, t_wait = "typed", t + 1 + #lines[n] * 0.2
  elseif phase == "typed" then
    ret = field({"Return"}); ret:set_value(1); turns0 = f(68)
    phase, t_wait = "down", t + 0.2
  elseif phase == "down" then
    ret:clear_value(); phase = "busy"
  elseif phase == "busy" and f(68) > turns0 and f(12) == 2 then
    manager.machine.video:snapshot()
    print(string.format("SHOT turn_%d t=%.1f genTicks=%d genSteps=%d", n, t, f(40), f(44)))
    n, phase, t_wait = n + 1, "idle", t + 1
  end
end)

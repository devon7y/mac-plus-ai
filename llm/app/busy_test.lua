-- Events while Pixel replies (pollTyping): type "abc" during a reply (it must appear in the input
-- line while the status says thinking), then during a second reply press Command-N (New Chat stops
-- the reply). Works on the Plus and the SE keyboards, on disks with a header-only "Pixel Test" file.
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local MAGIC = 0x50584C21
local base, next_scan, i, wait_until, pending = nil, 30, 1, 0, nil
local function f(off) local v = mem:read_u32(base + off); if v >= 0x80000000 then v = v - 0x100000000 end; return v end
local function now() return manager.machine.time:as_double() end
local function shot(name) manager.machine.video:snapshot(); print(string.format("SHOT %s t=%.1f state=%d turns=%d ntok=%d",
  name, now(), f(12), f(68), f(36))) end
local NAMES = {Return = {"Return"}, Command = {"Command", "Command / Open Apple"}}
local function field(name)
  for _, n in ipairs(NAMES[name] or {name}) do
    for _, port in pairs(manager.machine.ioport.ports) do
      for fname, fl in pairs(port.fields) do
        if fname == n or (#name == 1 and fname:find("^" .. name .. "%s")) then return fl end
      end
    end
  end
end
local function key(name, dur) local k = field(name); k:set_value(1); pending = {release = k, at = now() + (dur or 0.15)} end
local turns0, ntok0 = 0, 0
local steps = {
  {function() return base and (f(12) == 2 or f(12) == 4) end, function() end, 1},
  {function() return true end, function() manager.machine.natkeyboard:post("hi pixel") end, 4},
  {function() return true end, function() turns0 = f(68); key("Return") end, 0.5},
  {function() return f(12) == 3 end, function() end, 2},
  {function() return true end, function() manager.machine.natkeyboard:post("abc") end, 3},
  {function() return true end, function() shot("busy1_abc_typed_while_thinking") end},
  {function() return f(68) > turns0 and f(12) == 2 end, function() shot("busy2_reply_done_abc_kept") end, 1},
  {function() return true end, function() key("Backspace"); end, 0.4},
  {function() return true end, function() key("Backspace"); end, 0.4},
  {function() return true end, function() key("Backspace"); end, 0.4},
  {function() return true end, function() manager.machine.natkeyboard:post("tell me a long story") end, 6},
  {function() return true end, function() turns0 = f(68); ntok0 = f(36); key("Return") end, 0.5},
  {function() return f(12) == 3 end, function() end, 6},
  {function() return true end, function() shot("busy3_before_cmd_n") end, 0.2},
  {function() return true end, function() field("Command"):set_value(1) end, 0.4},
  {function() return true end, function() key("n", 0.3) end, 0.6},
  {function() return true end, function() field("Command"):clear_value() end},
  {function() return f(12) == 2 end, function() shot("busy4_state2_after_cmd_n") end, 1},
  {function() return true end, function() shot("busy5_new_chat_1s_later") end, 0.5},
  {function() return true end, function()
    print(string.format("ABORT reply tokens before Cmd-N stopped it: %d, turns %d -> %d", f(36) - ntok0, turns0, f(68)))
    print("RESULT BUSY DONE"); manager.machine:exit()
  end},
}
NAMES.Backspace = {"Backspace", "Delete"}
sub = emu.add_machine_frame_notifier(function()
  local t = now()
  if pending and t >= pending.at then pending.release:clear_value(); pending = nil end
  if t > 2000 then print("RESULT TIMEOUT"); manager.machine:exit() return end
  if not base then
    if t >= next_scan then
      next_scan = t + 10
      for a = 0x8000, 0x3FFFF0, 2 do
        if mem:read_u16(a) == 0x5058 and mem:read_u32(a) == MAGIC and mem:read_u32(a + 8) == 0xAFA7B3DE then base = a; break end
      end
    end
    return
  end
  if t < wait_until or pending then return end
  local s = steps[i]
  if s and s[1]() then s[2](); wait_until = t + (s[3] or 0); i = i + 1 end
end)

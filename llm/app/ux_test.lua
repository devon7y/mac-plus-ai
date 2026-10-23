-- UX check of the release build: editing, typing while Pixel thinks, a long message, scrolling,
-- Cmd-Q. Screenshots in $RUN_DIR/shots/ (named in the log as "SHOT <name>").
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local MAGIC = 0x50584C21
local base, next_scan, i, wait_until = nil, 30, 1, 0
local function f(off) local v = mem:read_u32(base + off); if v >= 0x80000000 then v = v - 0x100000000 end; return v end
local function now() return manager.machine.time:as_double() end
local function shot(name) manager.machine.video:snapshot(); print(string.format("SHOT %s t=%.1f", name, now())) end
-- keys by name on any keyboard (Plus :kbd:usp:*, SE ADB :macadb:*), as emu/scripts/smoke_game.lua
local NAMES = {Return = {"Return"}, Backspace = {"Backspace", "Delete"}, Command = {"Command", "Command / Open Apple"}}
local function field(_, name)
  for _, n in ipairs(NAMES[name] or {name}) do
    for _, port in pairs(manager.machine.ioport.ports) do
      for fname, f in pairs(port.fields) do
        if fname == n or (#name == 1 and fname:find("^" .. name .. "%s")) then return f end
      end
    end
  end
end
local function key(port, name, dur) local k = field(port, name); k:set_value(1); return {release = k, at = now() + (dur or 0.15)} end
local pending = nil
local ports = manager.machine.ioport.ports
local btn = (ports[":macadb:MOUSE0"] and ports[":macadb:MOUSE0"].fields["Mouse Button 0"])
            or (ports[":MOUSE0"] and ports[":MOUSE0"].fields["Mouse Button"])
local function mouse_to(h, v)   -- absolute cursor move (global coordinates), as emu/scripts/lib.lua
  mem:write_u16(0x828, v); mem:write_u16(0x82A, h)
  mem:write_u16(0x82C, v); mem:write_u16(0x82E, h)
  mem:write_u8(0x8CE, mem:read_u8(0x8CF))
end
local function menu_pick(title_h, item_v)    -- press on a menu title, drag to an item, release
  return {
    {function() return true end, function() mouse_to(title_h, 8) end, 0.5},
    {function() return true end, function() btn:set_value(1) end, 0.8},
    {function() return true end, function() mouse_to(title_h + 12, item_v) end, 0.8},
    {function() return true end, function() btn:clear_value() end, 1.5},
  }
end
local turns0 = 0
local long_msg = "this is a very long message to see how the input line and the transcript wrap " ..
                 "when someone types more than one line"
local steps = {
  {function() return base and (f(12) == 2 or f(12) == 4) end, function() shot("ux1_ready") end},   -- 4: header-only test file
  {function() return true end, function() manager.machine.natkeyboard:post("hellp") end, 4},
  {function() return true end, function() pending = key(nil, "Backspace") end, 1},
  {function() return true end, function() manager.machine.natkeyboard:post("o") end, 2},
  {function() return true end, function() shot("ux2_backspace_hello") end, 1},
  {function() return true end, function() turns0 = f(68); pending = key(nil, "Return") end, 12},
  {function() return true end, function() manager.machine.natkeyboard:post("abc") end, 25},
  {function() return true end, function() shot("ux3_typing_while_thinking") end},
  {function() return f(68) > turns0 and f(12) == 2 end, function() shot("ux4_after_reply_input_kept") end, 1},
  {function() return true end, function() pending = key(nil, "Backspace") end, 0.5},
  {function() return true end, function() pending = key(nil, "Backspace") end, 0.5},
  {function() return true end, function() pending = key(nil, "Backspace") end, 0.5},
  {function() return true end, function() manager.machine.natkeyboard:post(long_msg) end, 40},
  {function() return true end, function() shot("ux5_long_input") end, 1},
  {function() return true end, function() turns0 = f(68); pending = key(nil, "Return") end, 5},
  {function() return f(68) > turns0 and f(12) == 2 end, function() shot("ux6_long_reply") end, 1},
}
for n, msg in ipairs({"tell me a joke", "what is your name", "do you like mice", "good night pixel"}) do
  steps[#steps + 1] = {function() return true end, function() manager.machine.natkeyboard:post(msg) end, 6}
  steps[#steps + 1] = {function() return true end, function() turns0 = f(68); pending = key(nil, "Return") end, 5}
  steps[#steps + 1] = {function() return f(68) > turns0 and f(12) == 2 end, function() shot("ux7_scroll_" .. n) end, 1}
end
-- menus: Apple > About Pixel..., then File > New Chat
steps[#steps + 1] = {function() return true end, function() shot("ux9_menubar") end, 1}
for _, s in ipairs(menu_pick(20, 28)) do steps[#steps + 1] = s end
steps[#steps + 1] = {function() return true end, function() shot("ux10_about") end, 1}
steps[#steps + 1] = {function() return true end, function() pending = key(nil, "Return") end, 2}
steps[#steps + 1] = {function() return true end, function() shot("ux11_after_about") end, 1}
for _, s in ipairs(menu_pick(52, 28)) do steps[#steps + 1] = s end
steps[#steps + 1] = {function() return true end, function() shot("ux12_new_chat") end, 1}
steps[#steps + 1] = {function() return true end, function() print(string.format("BEFORE QUIT state=%d turns=%d", f(12), f(68))) end, 1}
steps[#steps + 1] = {function() return true end, function() field(nil, "Command"):set_value(1) end, 0.5}
steps[#steps + 1] = {function() return true end, function()
  pending = key(nil, "q", 0.3)
  print("CMD-Q")
end, 0.6}
steps[#steps + 1] = {function() return true end, function() field(nil, "Command"):clear_value() end, 3}
steps[#steps + 1] = {function() return true end, function() shot("ux8_quit_3s") end, 20}
steps[#steps + 1] = {function() return true end, function() shot("ux8_quit_23s") end, 60}
steps[#steps + 1] = {function() return true end, function() shot("ux8_quit_83s") end, 60}
steps[#steps + 1] = {function() return true end, function()
  shot("ux8_quit_143s")
  if base then print(string.format("AFTER QUIT state=%d turns=%d", f(12), f(68))) end
  print("RESULT UX DONE"); manager.machine:exit()
end}

if os.getenv("QUICK") then   -- only the quit check: ready, then Cmd-Q and the shots after it
  local q = {steps[1]}
  for k = #steps - 23, #steps do q[#q + 1] = steps[k] end
  steps = q
end
sub = emu.add_machine_frame_notifier(function()
  local t = now()
  if pending and t >= pending.at then pending.release:clear_value(); pending = nil end
  if t > 4000 then print("RESULT TIMEOUT"); manager.machine:exit() return end
  if not base then
    if t >= next_scan then
      next_scan = t + 10
      for a = 0x8000, 0x3FFFF0, 2 do
        if mem:read_u16(a) == 0x5058 and mem:read_u32(a) == MAGIC and mem:read_u32(a + 8) == 0xAFA7B3DE then
          base = a; print(string.format("block at %06x t=%.0f", a, t)); break
        end
      end
    end
    return
  end
  if t < wait_until or pending then return end
  local s = steps[i]
  if s and s[1]() then
    s[2]()
    wait_until = t + (s[3] or 0)
    i = i + 1
  end
end)

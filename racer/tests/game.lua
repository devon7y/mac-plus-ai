-- Endurance scripted test. Finds the 'ENDU' debug block, then runs a scenario of timed
-- actions (relative to when the block is found), logging state and taking screenshots.
--   SCENARIO=drive|keys|days   (default drive)   SEED=<n>   LOG_EVERY=<s>
-- (self-contained: works on the Plus and on the SE, whose ADB mouse has no :MOUSE0 port)
mem = manager.machine.devices[":maincpu"].spaces["program"]
screen = manager.machine.screens[":screen"]
local mport = manager.machine.ioport.ports[":MOUSE0"]
btn = mport and mport.fields["Mouse Button"] or { set_value = function() end, clear_value = function() end }
function mouse_to(h, v)
  mem:write_u16(0x828, v); mem:write_u16(0x82A, h); mem:write_u16(0x82C, v); mem:write_u16(0x82E, h)
  mem:write_u8(0x8CE, mem:read_u8(0x8CF))
end
local scen = os.getenv("SCENARIO") or "drive"
local seed = tonumber(os.getenv("SEED") or "4242")
local log_every = tonumber(os.getenv("LOG_EVERY") or "5")
local base, t0, next_scan, next_log = nil, nil, 6, 0
local function scan()
  -- Retro68 keeps globals in the application heap: scan from ApplZone ($2AA) up to 1.5 MB
  local zone = mem:read_u32(0x2AA)
  local lo, hi = 0x1000, 0xFFFF0
  if zone > 0x1000 and zone < 0x1000000 then lo = zone; hi = math.min(zone + 0x180000, mem:read_u32(0x108) - 4) end
  for a = lo - lo % 2, hi, 2 do
    if mem:read_u32(a) == 0x454E4455 and mem:read_u32(a + 8) == 0xBAB1BBAA then return a end
  end
end
local function f(off, sz)
  if sz == 4 then return mem:read_u32(base + off) end
  return mem:read_i16(base + off)
end
local function cmd(c) mem:write_u16(base + 52, c) end
-- keys are found by MAME field name, so this works on the Plus keyboard and the SE's ADB one
local names = { left = {"Cursor Left", "Left Arrow"}, right = {"Cursor Right", "Right Arrow"},
  up = {"Cursor Up", "Up Arrow"}, down = {"Cursor Down", "Down Arrow"}, ret = {"Return"},
  space = {"Space"}, cmd = {"Command", "Command / Open Apple"}, esc = {"Esc", "`  ~"} }
local fcache = {}
local function field(name)
  if fcache[name] then return fcache[name] end
  local wants = names[name] or {}
  for _, want in ipairs(wants) do        -- first listed name that exists wins (Esc before `)
    for pname, port in pairs(manager.machine.ioport.ports) do
      for fname, f in pairs(port.fields) do
        if fname == want and not fcache[name] then fcache[name] = f; print("key " .. name .. " = " .. pname .. " " .. fname) end
      end
    end
  end
  if not names[name] and #name == 1 then
    for _, port in pairs(manager.machine.ioport.ports) do
      for fname, f in pairs(port.fields) do
        if fname:find("^" .. name:lower() .. "  ") then fcache[name] = f end
      end
    end
  end
  if not fcache[name] then print("NO KEY FIELD " .. name) end
  return fcache[name]
end
local function key(name, down)
  local fld = field(name)
  if not fld then return end
  if down then fld:set_value(1) else fld:clear_value() end
end
local mx, my = 224, 171
local steer = 0
local fpsmark = nil
local function state(tag)
  local stages = {"MORNING", "SNOW", "DUSK", "NIGHT", "FOG", "DAWN"}
  print(string.format("[%s t=%.1f] mode=%d day=%d quota=%d stage=%s(%d) speed=%d px=%d odo=%.1f passed=%d got=%d crash=%d fps=%.1f riv=%d in=%d keys=%d mdx=%d hud=%d best=%d",
    tag, manager.machine.time:as_double(), f(20), f(22), f(24), stages[f(26) + 1] or "?", f(64), f(28), f(30),
    f(32, 4) / 10, f(40), f(42), f(44), f(46) / 10, f(62), f(60), f(74), f(72), f(76), f(56, 4)))
end
local function fps_start() fpsmark = { f(12, 4), f(16, 4) } end
local function prof()
  local n = mem:read_u32(base + 108)
  if n == 0 then return end
  local names = {"geom", "draw", "sprites", "copy+mtn", "hud+snd", "logic"}
  local out = {}
  for i = 0, 5 do out[#out + 1] = string.format("%s %.2f", names[i + 1], mem:read_u32(base + 84 + 4 * i) / 60.15 * 1000 / n) end
  print(string.format("PROFILE ms/frame over %d frames: %s", n, table.concat(out, ", ")))
end
local function fps_end(tag)
  local fr, tk = f(12, 4) - fpsmark[1], f(16, 4) - fpsmark[2]
  print(string.format("FPS %s: %d frames in %d ticks = %.1f fps", tag, fr, tk, fr / (tk / 60.15)))
end
local S = {}
if scen == "drive" then
  S = {
    {1, function() state("title"); screen:snapshot("g_title.png") end},
    {2, function() mem:write_u32(base + 48, seed); cmd(1) end},
    {3, function() btn:set_value(1); state("go") end},
    {10, function() fps_start(); state("cruise"); screen:snapshot("g_cruise.png") end},
    {20, function() fps_end("cruise 10s"); prof(); state("t20"); screen:snapshot("g_t20.png") end},
    {21, function() steer = 6 end},
    {24, function() steer = 0; state("steered right"); screen:snapshot("g_right.png") end},
    {25, function() steer = -6 end},
    {31, function() steer = 0; state("steered left"); screen:snapshot("g_left.png") end},
    {40, function() state("t40"); screen:snapshot("g_t40.png") end},
    {41, function() btn:set_value(0); key("z", true) end},
    {45, function() key("z", false); state("braked"); screen:snapshot("g_braked.png") end},
    {46, function() key("x", true); key("right", true) end},
    {49, function() key("right", false); state("keys right") end},
    {50, function() key("left", true) end},
    {53, function() key("left", false); state("keys left"); screen:snapshot("g_keys.png") end},
    {60, function() state("end"); manager.machine:exit() end},
  }
elseif scen == "shell" then
  -- title -> Return starts -> day ends unmet -> GAME OVER -> click -> title -> click starts
  S = {
    {1, function() state("title"); screen:snapshot("s_title.png") end},
    {2, function() key("ret", true) end},
    {2.3, function() key("ret", false) end},
    {4, function() state("after Return") end},
    {5, function() btn:set_value(1) end},
    {8, function() key("p", true) end},
    {8.3, function() key("p", false); pf = f(12, 4) end},
    {10, function() print(string.format("PAUSE frames advanced while paused: %d", f(12, 4) - pf)) end},
    {10.5, function() key("p", true) end},
    {10.8, function() key("p", false) end},
    {11.8, function() print(string.format("PAUSE frames after resume: %d", f(12, 4) - pf)) end},
    {12, function() state("driving"); screen:snapshot("s_drive.png"); cmd(6) end},
    {13, function() btn:set_value(0); state("day ended") end},
    {20, function() state("game over"); screen:snapshot("s_over.png") end},
    {21, function() btn:set_value(1) end},
    {21.3, function() btn:set_value(0) end},
    {23, function() state("after click"); screen:snapshot("s_title2.png") end},
    {24, function() btn:set_value(1) end},
    {24.3, function() btn:set_value(0) end},
    {26, function() state("after 2nd click") end},
    {27, function() key("cmd", true); key("q", true) end},
    {27.3, function() key("q", false); key("cmd", false) end},
    {28.5, function() state("Cmd-Q in race -> title") end},
    {29, function() btn:set_value(1) end},
    {29.3, function() btn:set_value(0) end},
    {30.5, function() state("click -> race") end},
    {31, function() key("esc", true) end},
    {31.2, function() key("esc", false) end},
    {32.5, function() state("Esc in race -> title"); screen:snapshot("s_title3.png");
       print(string.format("controls line width %d px", f(128))) end},
    {33, function() key("esc", true) end},
    {33.2, function() key("esc", false); quit_t = manager.machine.time:as_double() end},
    {46, function() screen:snapshot("s_after_quit.png"); relaunch = true end},
    {62, function() screen:snapshot("s_after_quit2.png"); relaunch = true end},
  }
elseif scen == "wasd" then
  -- standard keys, held through MAME's keyboard input fields (no RAM pokes)
  local s0
  S = {
    {1, function() state("title") end},
    {2, function() key("ret", true) end},
    {2.3, function() key("ret", false) end},
    {4, function() state("started"); key("w", true) end},
    {9, function() state("W held 5 s") ; s0 = f(30); key("d", true) end},
    {11, function() state("W+D held 2 s"); print(string.format("WASD W+D: speed %d, px %d -> %d (%s)", f(28), s0, f(30), f(30) > s0 and "right" or "NOT RIGHT")); s0 = f(30); key("d", false); key("a", true) end},
    {13, function() state("W+A held 2 s"); print(string.format("WASD W+A: speed %d, px %d -> %d (%s)", f(28), s0, f(30), f(30) < s0 and "left" or "NOT LEFT")); key("a", false); s0 = f(28); key("w", false); key("s", true) end},
    {15, function() state("S held 2 s"); print(string.format("WASD S: speed %d -> %d (%s)", s0, f(28), f(28) < s0 / 3 and "braked" or "NOT BRAKED")); key("s", false); screen:snapshot("w_after.png") end},
    {16, function() print(string.format("WASD sound on before M: %d", f(136))); key("m", true) end},
    {16.3, function() key("m", false) end},
    {17, function() print(string.format("WASD sound on after M: %d", f(136))); key("m", true) end},
    {17.3, function() key("m", false) end},
    {18, function() print(string.format("WASD sound on after 2nd M: %d", f(136))); manager.machine:exit() end},
  }
elseif scen == "esc" then
  S = {
    {1, function() state("title") end},
    {2, function() key("ret", true) end},
    {2.3, function() key("ret", false) end},
    {4, function() state("race (Return)"); key("esc", true) end},
    {4.2, function() key("esc", false) end},
    {6, function() state("Esc in race -> title"); key("esc", true) end},
    {6.2, function() key("esc", false); quit_t = manager.machine.time:as_double() end},
    {20, function() screen:snapshot("e_after_quit.png"); relaunch = true end},
    {40, function() screen:snapshot("e_after_quit2.png"); relaunch = true end},
  }
elseif scen == "boot" then
  S = { {2, function() state("boot"); screen:snapshot("b_title.png"); manager.machine:exit() end} }
elseif scen == "days" then
  -- autopilot plays; checks stage/day transitions; snapshots of every stage
  S = { {1, function() mem:write_u32(base + 48, seed); mem:write_u16(base + 54, 1); cmd(1) end},
        {5, function() fps_start() end} }
  local last_stage, shots = -1, 0
  S[#S + 1] = {6, function()
    sub2 = emu.add_machine_frame_notifier(function()
      local st = f(26)
      if st ~= last_stage then
        last_stage = st
        local tick_in = f(64)
        state("stage")
      end
    end) end}
  local function daynet()
    local q = {50, 70, 85, 100}
    local out = {}
    for d = 0, 3 do
      local n = f(112 + 2 * d)
      if n ~= 0 then out[#out + 1] = string.format("day %d: net %d / quota %d = margin %+.0f%%", d + 1, n, q[d + 1], (n / q[d + 1] - 1) * 100) end
    end
    if #out > 0 then print("DAYNET " .. table.concat(out, "; ")) end
  end
  for i = 1, 200 do S[#S + 1] = {6 + i * 30, function() daynet()
    state("p"); if (i % 2) == 0 then screen:snapshot(string.format("d_%03d.png", i)) end
    if i % 4 == 0 then fps_end("window"); fps_start(); prof() end
  end} end
end
local i = 1
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not base then
    if t >= next_scan then
      base = scan(); next_scan = t + 2
      if base then t0 = t; print(string.format("ENDU at %06x (t=%.1fs)", base, t)) end
    end
    if t > tonumber(os.getenv("BLOCK_TIMEOUT") or "90") then print("TIMEOUT no block"); manager.machine:exit() end
    return
  end
  if relaunch then
    relaunch = false
    local old = base
    base = scan()
    if base then
      print(string.format("RELAUNCH block at %06x (was %06x) mode=%d frames=%d, %.1f s after quit", base, old, f(20),
        f(12, 4), t - quit_t))
    else
      print("RELAUNCH no block found")
    end
    relaunches = (relaunches or 0) + 1
    if relaunches >= 2 then manager.machine:exit() end
    return
  end
  if steer ~= 0 then mouse_to(mx + steer, my) end
  while i <= #S and t - t0 >= S[i][1] do S[i][2](); i = i + 1 end
end)

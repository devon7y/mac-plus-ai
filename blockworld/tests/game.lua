-- Blockworld M2-M4 game test (walk, mine and place, save and reload): MAME's own keyboard fields (W, Space, V, arrows, Esc) and the mouse
-- globals; state from the 'BWGM' debug block. Screenshots in $RUN_DIR/shots.
--   RAM=4M [SLOW=1.5] emu/run_test.sh blockworld/build/blockworld.img blockworld/tests/game.lua 400
local cpu = manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local screen = manager.machine.screens[":screen"]
local MAGIC, NOTMAGIC = 0x4257474D, 0xBDA8B8B2
local blk, next_scan, fails = nil, 40, 0
local function check(ok, what)
  print(string.format("CHECK %-4s %s", ok and "ok" or "FAIL", what))
  if not ok then fails = fails + 1 end
end
local function scan()
  for a = 0x1000, mem:read_u32(0x108) - 8, 2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == NOTMAGIC then return a end
  end
end
local ports = manager.machine.ioport.ports
local function find_named(names)
  for _, want in ipairs(names) do
    for _, port in pairs(ports) do
      for name, f in pairs(port.fields) do if name == want then return f end end
    end
  end
end
local function find_char(c)
  for _, port in pairs(ports) do
    for name, f in pairs(port.fields) do if name:find("^" .. c .. "  ") then return f end end
  end
end
local BTN = ports[":MOUSE0"] and ports[":MOUSE0"].fields["Mouse Button"]
local KEYS = {
  q = find_char("q"),
  w = find_char("w"), a = find_char("a"), d = find_char("d"), v = find_char("v"), e = find_char("e"),
  two = find_named({"2  @", "2  \""}) or find_char("2"),
  space = find_named({"Space"}), left = find_named({"Cursor Left", "Left Arrow"}),
  up = find_named({"Cursor Up", "Up Arrow"}), esc = find_named({"Esc"}) or find_char("`"),
}
for k, f in pairs(KEYS) do if not f then print("RESULT FAIL (no input field for key " .. k .. ")"); manager.machine:exit() end end
if not BTN then print("RESULT FAIL no mouse button field"); manager.machine:exit() end
print("key '2' field: " .. KEYS.two.name .. " " .. KEYS.two.port.tag)
local held = {}
local function hold(k, t, secs) KEYS[k]:set_value(1); held[k] = t + secs end
local function rd(o) return mem:read_i32(blk + o) end
local function rdu(o) return mem:read_u32(blk + o) end
local function rd16(o) return mem:read_i16(blk + o) end
local function pos() return string.format("x %.2f y %.2f z %.2f yaw %d pitch %d ground %d frames %d (moving %d, refines %d) autojumps %d",
  rd(16) / 256, rd(20) / 256, rd(24) / 256, rd16(28), rd16(30), rd16(78), rdu(32), rdu(36), rdu(40), rd16(66)) end
local function mouse_move(dx, dy)          -- the Mac's cursor moves by (dx, dy) from the centre
  local h, v = mem:read_i16(0x82A), mem:read_i16(0x828)
  mem:write_u16(0x828, v + dy); mem:write_u16(0x82A, h + dx)
  mem:write_u16(0x82C, v + dy); mem:write_u16(0x82E, h + dx)
  mem:write_u8(0x8CE, mem:read_u8(0x8CF))
end
local phase, t0, s = "boot", 0, {}
local seed_ok, seed_asked
local function to(p, t) phase = p; t0 = t; print(string.format("t=%.1f phase %s: %s", t, p, pos())) end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  for k, until_t in pairs(held) do
    if t >= until_t then KEYS[k]:clear_value(); held[k] = nil end
  end
  if not blk then
    if t >= next_scan then
      blk = scan(); next_scan = t + 5
      if blk then print(string.format("BWGM at %06x (t=%.1f)", blk, t)) end
    end
    if t > 300 and phase == "boot" then print("RESULT FAIL no debug block"); manager.machine:exit() end
    return
  end
  local st = rdu(8)
  if phase == "boot" then
    if st == 0xDEAD then print("RESULT FAIL out of memory"); manager.machine:exit() end
    if st == 2 and rdu(40) >= 1 and not seed_ok then           -- (M7: the game starts in its shipped
      if rd(292) == 2 and rd16(296) >= 1 then seed_ok = true   --  world; this test was written for
      elseif not seed_asked and mem:read_u16(blk + 162) == 0 then   --  seed 2's: make it first)
        print(string.format("  the game started in world %d (loaded %d, load %.1f s): asking for seed 2's", rd(292), rd16(152), rdu(158) / 60.15))
        mem:write_u16(blk + 164, 2); mem:write_u16(blk + 166, 1); mem:write_u16(blk + 162, 11); seed_asked = true
      end
      return
    end
    if st == 2 and rdu(40) >= 1 then
      mem:write_u16(blk + 214, 1); mem:write_u16(blk + 258, 1)   -- (no natural spawning, drops come at once)
      print(string.format("RESULT world made in %.1f s (Mac ticks); FreeMem %d; view %d", rdu(12) / 60.15, rd(72), rd16(76)))
      screen:snapshot("start.png")
      s.x, s.z, s.yaw, s.mv = rd(16), rd(24), rd16(28), rdu(36)
      s.mt, s.mc = rdu(48), rdu(52)
      hold("w", t, 4.0)
      to("walk", t)
    end
  elseif phase == "walk" and t - t0 > 4.5 then
    local dx, dz = (rd(16) - s.x) / 256, (rd(24) - s.z) / 256
    local fwd = dx * math.cos(s.yaw * math.pi / 512) + dz * math.sin(s.yaw * math.pi / 512)
    check(fwd > 1.0, string.format("W walks forward (yaw %d): moved %.2f blocks along it (%.2f, %.2f)", s.yaw, fwd, dx, dz))
    screen:snapshot("walked.png")
    s.yaw = rd16(28)
    s.mh, s.mv = mem:read_i16(0x82A), mem:read_i16(0x828)
    mouse_move(64, 0)
    to("mouse", t)
  elseif phase == "mouse" and t - t0 <= 1.5 then     -- (a write between the game's read and re-centre is lost)
    if rd16(28) == s.yaw and (s.yr or 0) < 30 and mem:read_i16(0x82A) == s.mh then s.yr = (s.yr or 0) + 1; mouse_move(64, 0) end
  elseif phase == "mouse" and t - t0 > 1.5 then
    check(((rd16(28) - s.yaw) & 1023) == 128, string.format("mouse 64 px right turns 128 (45 deg): yaw %d -> %d", s.yaw, rd16(28)))
    s.p = rd16(270)
    mouse_move(0, -30)
    to("look", t)
  elseif phase == "look" and t - t0 <= 1.5 then
    if rd16(270) == s.p and (s.lr or 0) < 30 and mem:read_i16(0x828) == s.mv then s.lr = (s.lr or 0) + 1; mouse_move(0, -30) end
  elseif phase == "look" and t - t0 > 1.5 then
    check(rd16(270) == s.p + 60, string.format("mouse 30 px up looks up 60 (21 deg; the view %d rows): aim %d -> %d", rd16(30), s.p, rd16(270)))
    mouse_move(0, 30)
    s.y = rd(20)
    s.maxy = rd(20)
    hold("space", t, 0.3)
    s.rf0 = rdu(40)
    to("jump", t)
  elseif phase == "jump" then
    if rd(20) > s.maxy then s.maxy = rd(20) end
    if t - t0 > 2.0 then
      check(s.maxy - s.y >= 256, string.format("Space jumps: feet rose %.2f blocks, back to %.2f", (s.maxy - s.y) / 256, (rd(20) - s.y) / 256))
      s.rf = s.rf0
      to("still", t)
    end
  elseif phase == "still" and t - t0 > 1.0 and not s.mv2 then
    s.mv2 = rdu(36)
  elseif phase == "still" and t - t0 > 4.0 then
    check(rdu(36) == s.mv2 and rdu(40) > s.rf, string.format("standing still: no moving frames; refined after landing (%.0f ms)", rdu(84) / 60.15 * 1000))
    screen:snapshot("still.png")
    s.aj = rd16(66)
    s.x, s.z = rd(16), rd(24)
    hold("w", t, 12.0)
    hold("a", t + 4, 2.0)
    to("explore", t)
  elseif phase == "explore" and t - t0 > 12.5 then
    screen:snapshot("explored.png")
    print(string.format("RESULT explored: %s", pos()))
    local mc = rdu(52) - s.mc
    local ms = (rdu(48) - s.mt) / 60.15 / math.max(mc, 1) * 1000
    print(string.format("RESULT moving frames: %d, average %.1f ms = %.2f fps (render + present, Mac ticks)", mc, ms, 1000 / ms))
    s.dm = rd16(76)
    hold("v", t, 0.3)
    to("view", t)
  elseif phase == "view" and t - t0 > 3.0 then
    check(rd16(76) ~= s.dm, string.format("V changes the view distance: %d -> %d", s.dm, rd16(76)))
    screen:snapshot("view.png")
    hold("v", t, 0.3)                      -- 8 -> 10 -> 12 again
    mem:write_u16(blk + 164, 64); mem:write_u16(blk + 166, 54); mem:write_u16(blk + 168, 0)
    mem:write_u16(blk + 162, 2)            -- (stand on plain grass: the walk may end at a tree)
    to("aim0", t)
  elseif phase == "aim0" and t - t0 > 1.5 then
    mouse_move(0, 40)                      -- look down at the ground in front (28 degrees)
    to("aim", t)
  elseif phase == "aim" and t - t0 <= 3.0 then   -- (a mouse write landing between the game's read
    if rd16(30) == 0 and (s.retry or 0) < 30 and mem:read_i16(0x828) == 171 then   -- and re-centre is lost)
      s.retry = (s.retry or 0) + 1; mouse_move(0, 40)
    end
  elseif phase == "aim" and t - t0 > 3.0 then
    if s.v2 == nil then hold("v", t, 0.3); s.v2 = 1 end
    print(string.format("  picked %d: block (%d, %d, %d) face %d, mined %d", rd16(88), rd16(90), rd16(92), rd16(94), rd16(96), rdu(100)))
    screen:snapshot("aim.png")
    check(rd16(88) == 1, "the crosshair picks a block when looking down")
    s.hit = {rd16(90), rd16(92), rd16(94)}
    s.mined = rdu(100)
    BTN:set_value(1)
    to("mine", t)
  elseif phase == "mine" and rdu(100) > s.mined and not s.released then
    BTN:clear_value(); s.released = true      -- (one block: the game no longer stalls on the V key)
  elseif phase == "mine" and t - t0 > 0.4 and not s.shot then
    screen:snapshot("mining.png"); s.shot = true
  elseif phase == "mine" and t - t0 > 2.5 then
    BTN:clear_value()
    s.release = t
    local items = mem:read_u8(blk + 108) .. "x" .. mem:read_u8(blk + 109)
    check(rdu(100) > s.mined and mem:read_u8(blk + 109) >= 1,
      string.format("holding the button mines it: mined %d -> %d, hotbar slot 1 %s", s.mined, rdu(100), items))
    screen:snapshot("mined.png")
    to("settle", t)
  elseif phase == "settle" and t - t0 > 1.0 then
    s.placed = rdu(104)
    s.count = mem:read_u8(blk + 109)
    hold("e", t, 0.3)
    to("place", t)
  elseif phase == "place" and t - t0 > 2.0 then
    check(rdu(104) == s.placed + 1 and mem:read_u8(blk + 109) == s.count - 1,
      string.format("E places it: placed %d -> %d, count %d -> %d", s.placed, rdu(104), s.count, mem:read_u8(blk + 109)))
    screen:snapshot("placed.png")
    hold("two", t, 0.3)
    to("select", t)
  elseif phase == "select" and t - t0 > 1.5 then
    check(rd16(144) == 1, string.format("2 selects the second slot: selected %d", rd16(144)))
    hold("esc", t, 0.5)
    to("menu", t)
  elseif phase == "menu" and t - t0 > 1.5 then
    hold("q", t, 0.5)
    to("quit", t)
  elseif phase == "quit" and t - t0 > 6.0 and not s.q then
    s.q = true
    s.qx, s.qz, s.qhot = rd(16), rd(24), mem:read_u8(blk + 108) * 256 + mem:read_u8(blk + 109)
    screen:snapshot("quit.png")
    blk = nil                              -- the game saves, quits and is started again
    next_scan = t + 5
    phase = "reload"; t0 = t
    print(string.format("t=%.1f phase reload", t))
  elseif phase == "reload" and rd16(152) ~= 1 and t - (s.rs or t0) > 5 then
    print(string.format("  t=%.1f block: state %d loaded %d loadErr %d saveErr %d frames %d", t, rdu(8), rd16(152), rd16(154), rd16(156), rdu(32)))
    s.rs = t; blk = nil; next_scan = t                      -- (an old copy of the block: look again)
  elseif phase == "reload" and rd16(152) == 1 and rdu(8) == 2 and rdu(40) >= 1 then
    check(rd16(152) == 1, string.format("restarted from the save: loaded %d (error %d), load %.1f s, saved with error %d",
      rd16(152), rd16(154), rdu(158) / 60.15, rd16(156)))
    check(rd(16) == s.qx and rd(24) == s.qz, string.format("the player is where it was: (%.2f, %.2f) vs (%.2f, %.2f)",
      rd(16) / 256, rd(24) / 256, s.qx / 256, s.qz / 256))
    screen:snapshot("reloaded.png")
    print(string.format("RESULT %s (%d failed checks)", fails == 0 and "PASS" or "FAIL", fails))
    manager.machine:exit()
  elseif phase == "reload" and t - t0 > 150 then
    check(false, "the game came back after Esc")
    print(string.format("RESULT FAIL (%d failed checks)", fails))
    manager.machine:exit()
  end
end)

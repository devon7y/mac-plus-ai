-- Blockworld mobs test: every kind drawn, melee both ways, a sizzler blowing up, an archer
-- shooting, the bow, natural spawning by night and by day. Natural spawning is off (debug block)
-- except where tested.  RAM=4M emu/run_test.sh blockworld/build/blockworld.img blockworld/tests/mobs.lua 600
local cpu = manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local screen = manager.machine.screens[":screen"]
local MAGIC, NOTMAGIC = 0x4257474D, 0xBDA8B8B2
local blk, next_scan, fails = nil, 40, 0
DEBUG_STOP = os.getenv("DEBUG_STOP")
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
local BTN = ports[":MOUSE0"].fields["Mouse Button"]
local KEYS = {tab = find_named({"Tab"}), e = find_char("e"), esc = find_named({"Esc"}) or find_char("`"),
  shift = find_named({"Shift", "Left Shift"}), one = find_char("1"), two = find_char("2"), three = find_char("3")}
for k, f in pairs(KEYS) do if not f then print("RESULT FAIL (no key " .. k .. ")"); manager.machine:exit() end end

-- items (gen_items.py): blocks keep their world.h numbers
local I = {log = 5, boards = 6, rubble = 4, rock = 3, coal_ore = 10, iron_ore = 11, bench = 14, chest = 17,
  furnace = 18, stick = 32, coal = 33, iron_bar = 34, rock_pick = 64 + 5 + 0, wood_pick = 64}
local function rd16(o) return mem:read_i16(blk + o) end
local function rdu(o) return mem:read_u32(blk + o) end
local function slot(i)          -- inventory slot i: item, count, uses left
  local a = rdu(180) + 4 * i
  return mem:read_u8(a), mem:read_u8(a + 1), mem:read_u16(a + 2)
end
local function count(item)
  local n = 0
  for i = 0, 35 do local it, c = slot(i); if it == item then n = n + c end end
  return n
end
local function give(item, n) mem:write_u16(blk + 164, item); mem:write_u16(blk + 166, n); mem:write_u16(blk + 162, 1) end
local function mouse_to(h, v)
  mem:write_u16(0x828, v); mem:write_u16(0x82A, h); mem:write_u16(0x82C, v); mem:write_u16(0x82E, h)
  mem:write_u8(0x8CE, mem:read_u8(0x8CF))
end
-- slot centres (global): see inv.c panel_run
local function px(x, y) return rd16(176) + x + 11, rd16(178) + y + 11 end
local function hot(i) return px(36 + 24 * i, 174) end
local function pack(i) return px(36 + 24 * (i % 9), 98 + 24 * (i // 9)) end
local function grid2(i) return px(120 + 24 * (i % 2), 24 + 24 * (i // 2)) end
local function grid3(i) return px(72 + 24 * (i % 3), 18 + 24 * (i // 3)) end
local function out2() return px(204, 36) end
local function out3() return px(204, 42) end
local function chest_slot(i) return px(36 + 24 * (i % 9), 18 + 24 * (i // 9)) end
local function furn_in() return px(96, 18) end
local function furn_fuel() return px(96, 66) end
local function furn_out() return px(180, 42) end

-- a script of actions, run one per few frames
local acts, ai, wait_until = {}, 1, 0
local function A(f, secs) acts[#acts + 1] = {f, secs or 0.25} end
local function key(k, secs) A(function() KEYS[k]:set_value(1) end, secs or 0.3); A(function() KEYS[k]:clear_value() end, 0.4) end
local function clickat(fn, ...) local args = {...}
  A(function() mouse_to(fn(table.unpack(args))) end, 0.2)
  A(function() BTN:set_value(1) end, 0.15); A(function() BTN:clear_value() end, 0.3) end
local function shiftclick(fn, ...) local args = {...}
  A(function() KEYS.shift:set_value(1); mouse_to(fn(table.unpack(args))) end, 0.2)
  A(function() BTN:set_value(1) end, 0.15); A(function() BTN:clear_value() end, 0.2)
  A(function() KEYS.shift:clear_value() end, 0.3) end
local function note(f) A(f, 0) end

-- helpers working on the live inventory
local function find(item) for i = 0, 35 do local it, c = slot(i); if it == item and c > 0 then return i end end end
local function empty() for i = 35, 9, -1 do local it, c = slot(i); if c == 0 then return i end end end
local function inv_pos(i) if i < 9 then return hot(i) else return pack(i - 9) end end
local saved = {}
local function pick_up(item)     -- the whole stack into the hand (remembers where it was)
  A(function() saved[item] = find(item); mouse_to(inv_pos(saved[item])) end, 0.2)
  A(function() BTN:set_value(1) end, 0.15); A(function() BTN:clear_value() end, 0.3)
end
local function put_back(item)
  A(function() mouse_to(inv_pos(saved[item])) end, 0.2)
  A(function() BTN:set_value(1) end, 0.15); A(function() BTN:clear_value() end, 0.3)
end
local OPT = find_named({"Option", "Left Option", "Alt"})
local function one_into(fn, i)   -- option-click: one item from the hand
  A(function() OPT:set_value(1); mouse_to(fn(i)) end, 0.2)
  A(function() BTN:set_value(1) end, 0.15); A(function() BTN:clear_value() end, 0.15)
  A(function() OPT:clear_value() end, 0.25)
end
local function put_hand_away()   -- the hand into an empty pack slot
  A(function() mouse_to(inv_pos(empty())) end, 0.2)
  A(function() BTN:set_value(1) end, 0.15); A(function() BTN:clear_value() end, 0.3)
end
local function select_item(item) -- the hotbar slot holding item: its number key
  A(function()
    local i = find(item)
    if i and i < 9 then local k = find_char(tostring(i + 1)); k:set_value(1); sel_key = k
    else print("  (not in the hotbar: item " .. item .. ")") end
  end, 0.3)
  A(function() if sel_key then sel_key:clear_value(); sel_key = nil end end, 0.4)
end
local look_dy, look_p0, look_n, look_tk, look_loops = nil, 0, 0, 0, 0
local function look_write(dy)
  local v, h = mem:read_i16(0x828), mem:read_i16(0x82A)
  mem:write_u16(0x828, v + dy); mem:write_u16(0x82A, h); mem:write_u16(0x82C, v + dy); mem:write_u16(0x82E, h)
  mem:write_u8(0x8CE, mem:read_u8(0x8CF))
end
-- the game reads the mouse and re-centres it; a write landing between the two is lost, so the
-- delta is written again (each frame, up to 30 times) until the pitch moves
local function look_frame()                -- (again only if the game ran two loops since and the
  if look_dy and rd16(30) ~= look_p0 then look_dy = nil end           --  pitch is the same: its copy lags)
  if look_dy and rdu(56) ~= look_tk then look_tk = rdu(56); look_loops = look_loops + 1 end
  if look_dy and look_n < 30 and look_loops >= 2 and mem:read_i16(0x828) == 171 then
    look_n = look_n + 1; look_loops = 0; look_write(look_dy)
  end
end
-- the tests were written for 2 rows of pitch a mouse pixel; the mouse now turns the aim (2/1024 of
-- a turn a pixel; the view's pitch is 256 tan(aim) rows): the pixels that give the rows asked for
local function old_px(dy)
  local want = math.floor(math.atan((rd16(30) - 2 * dy) / 256) * 512 / math.pi + 0.5)
  local px = (rd16(270) - want) / 2
  return px >= 0 and math.floor(px + 0.5) or -math.floor(-px + 0.5)
end
local function look(dy)
  A(function()
    local v, h = mem:read_i16(0x828), mem:read_i16(0x82A)
    print(string.format("  look %d: pitch %d, MTemp (%d, %d) Mouse (%d, %d)", dy, rd16(30), v, h, mem:read_i16(0x830), mem:read_i16(0x832)))
    local px = old_px(dy)
    look_dy, look_p0, look_n, look_tk, look_loops = px, rd16(30), 0, rdu(56), 0
    look_write(px)
  end, 1.0)
  A(function() look_dy = nil; print(string.format("  -> pitch %d, MTemp (%d, %d)", rd16(30), mem:read_i16(0x828), mem:read_i16(0x82A))) end, 0.1)
end

local function cmd(c, a0, a1, a2) mem:write_u16(blk + 164, a0 or 0); mem:write_u16(blk + 166, a1 or 0); mem:write_u16(blk + 168, a2 or 0); mem:write_u16(blk + 162, c) end
local NAMES = {"yak", "boar", "ram", "fowl", "shambler", "archer", "crawler", "sizzler"}
local function mob(i)            -- kind, alive, hp, x, y, z (blocks)
  local a = rdu(216) + 36 * i
  return mem:read_u8(a + 20), mem:read_u8(a + 21), mem:read_i16(a + 24), mem:read_i32(a) / 256, mem:read_i32(a + 4) / 256, mem:read_i32(a + 8) / 256
end
local function alive(kind)       -- how many (of a kind, or hostile / passive with -1 / -2)
  local n = 0
  for i = 0, 11 do
    local k, al = mob(i)
    if al == 1 and (k == kind or (kind == -1 and k >= 4) or (kind == -2 and k < 4)) then n = n + 1 end
  end
  return n
end
local function first(kind) for i = 0, 11 do local k, al = mob(i); if al == 1 and k == kind then return i end end end
local function health() return rd16(200) end
local S = {}

note(function() mem:write_u16(blk + 214, 1) end)                       -- no natural spawning
note(function() cmd(2, 64, 64, 0) end); A(function() end, 1.0)
note(function() cmd(7) end); A(function() end, 0.5)
-- every kind, side on, 4 blocks ahead
for k = 0, 7 do
  note(function() cmd(6, k, 4, 256) end); A(function() end, 1.2)
  note(function() check(alive(k) == 1, string.format("a %s stands in front", NAMES[k + 1])); screen:snapshot("mob_" .. NAMES[k + 1] .. ".png") end)
  note(function() cmd(7) end); A(function() end, 0.3)
end
-- front views: a yak and a shambler facing the player (the shambler at day in the sun burns: noted)
note(function() cmd(6, 0, 3, 512) end); A(function() end, 1.0)
note(function() screen:snapshot("mob_yak_front.png"); cmd(7) end); A(function() end, 0.3)

-- melee at night: a shambler walks up and hits; a gem sword kills it in three hits
note(function() cmd(3, 1500) end); A(function() end, 1.0)
note(function() cmd(4, 20, 20) end); A(function() end, 0.3)
note(function() give(87, 1) end); A(function() end, 0.5)              -- gem sword
select_item(87)
note(function() cmd(6, 4, 5, 512) end); A(function() end, 4.0)
note(function() S.h = health(); check(S.h < 20, string.format("the shambler came and hit (health %d)", S.h)); screen:snapshot("mob_shambler_night.png") end)
note(function() S.k = rdu(220); S.meat = count(44); print(string.format("  target %d", rd16(232))) end)
A(function() BTN:set_value(1) end, 2.0); A(function() BTN:clear_value() end, 0.5)
note(function() check(alive(4) == 0 and rdu(220) == S.k + 1, string.format("three sword hits kill it (kills %d -> %d)", S.k, rdu(220))) end)

-- a sizzler: comes close, hisses, blows up a crater and hurts
note(function() cmd(4, 20, 20) end); A(function() end, 0.3)
note(function() cmd(7) end); A(function() end, 0.5)
note(function() S.b = rd16(238); cmd(6, 7, 4, 512) end); A(function() end, 1.5)
shotf = function(n) note(function() screen:snapshot(n) end) end
shotf("mob_sizzler_close.png")
A(function() end, 5.0)
note(function() check(rd16(238) == S.b + 1 and health() < 20, string.format("the sizzler blew up (explosions %d, health %d)", rd16(238), health())); screen:snapshot("mob_crater.png") end)

-- an archer 9 blocks away shoots
note(function() cmd(4, 20, 20) end); A(function() end, 0.3)
note(function() cmd(7) end); A(function() end, 0.5)
note(function() cmd(2, 64, 64, 0) end); A(function() end, 1.0)
note(function() cmd(6, 5, 9, 512) end); A(function() end, 1.5)
shotf("mob_archer_shooting.png")
A(function() end, 7.0)
note(function() check(health() < 20, string.format("the archer's arrows hit (health %d)", health())) end)

-- the bow: an arrow into a boar 5 blocks ahead
note(function() cmd(4, 20, 20) end); A(function() end, 0.3)
note(function() cmd(7) end); A(function() end, 0.5)
note(function() give(50, 1) end); A(function() end, 0.5)
note(function() give(49, 8) end); A(function() end, 0.5)
select_item(50)
note(function() cmd(2, 64, 54, 0) end); A(function() end, 1.5)
note(function() cmd(6, 1, 5, 256) end); A(function() end, 1.0)
look(30)                                                              -- aim a little down
key("e", 0.2)
shotf("mob_arrow.png")
A(function() end, 1.5)
note(function() local i = first(1); local hp = i and select(3, mob(i)) or 0
  check(i and hp < 10 and count(49) == 7, string.format("an arrow hits the boar (hp %d, arrows %d)", hp, count(49))) end)

-- natural spawning: monsters at night, animals by day
note(function() cmd(7); mem:write_u16(blk + 214, 0) end); A(function() end, 0.3)
A(function() end, 20.0)
note(function() S.night = alive(-1); check(S.night > 0, string.format("night: %d monsters spawned (animals %d)", S.night, alive(-2))); screen:snapshot("mob_night_spawns.png") end)
note(function() cmd(7) end); A(function() end, 0.3)
note(function() cmd(3, 600) end); A(function() end, 20.0)
note(function() check(alive(-2) > 0, string.format("day: %d animals spawned (monsters %d)", alive(-2), alive(-1))); screen:snapshot("mob_day_spawns.png") end)
note(function() print(string.format("  kills %d, sprite-only redraws %d", rdu(220), rdu(224))) end)
-- mobs are saved: two animals standing, Esc + Q, the game restarts with them
note(function() KEYS.q = find_char("q") end)
note(function() cmd(7); mem:write_u16(blk + 214, 1) end); A(function() end, 0.5)
note(function() cmd(6, 0, 4, 256) end); A(function() end, 0.5)
note(function() cmd(6, 2, 6, 768) end); A(function() end, 1.5)
note(function() S.saved = {}
  for i = 0, 11 do local k, al, hp, x, y, z = mob(i); if al == 1 then S.saved[#S.saved + 1] = {k, x, z} end end end)
key("esc", 1.0)
key("q", 1.0)
note(function() blk = nil; next_scan = manager.machine.time:as_double() + 8; reloading = true end)
note(function()
  local found = 0
  for _, m in ipairs(S.saved) do
    for i = 0, 11 do local k, al, hp, x, y, z = mob(i)
      if al == 1 and k == m[1] and math.abs(x - m[2]) < 0.6 and math.abs(z - m[3]) < 0.6 then found = found + 1; break end
    end
  end
  check(#S.saved == 2 and found == 2, string.format("mobs are saved: %d of %d back after restarting (loaded %d)", found, #S.saved, rd16(152)))
end)
note(function() print("RESULT " .. (fails == 0 and "PASS" or "FAIL") .. string.format(" (%d failed checks)", fails)); manager.machine:exit() end)

local t_next = 0
local seed_ok, seed_asked
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not blk then
    if t >= next_scan then
      blk = scan(); next_scan = t + 5
      if blk then print(string.format("BWGM at %06x (t=%.1f)", blk, t)) end
    end
    if t > 600 then print("RESULT FAIL no debug block"); manager.machine:exit() end
    return
  end
  if reloading and mem:read_i16(blk + 152) ~= 1 then
    if t - (rescan_t or 0) > 5 then rescan_t = t; blk = nil; next_scan = t end
    return
  end
  if rdu(8) ~= 2 or rdu(40) < 1 then return end
  if not seed_ok then                               -- (M7: the game starts in its shipped world; these
    local ws = mem:read_i32(blk + 292)               --  tests were written for seed 2's: make it first)
    if ws == 2 and (mem:read_i16(blk + 296) >= 1 or mem:read_i16(blk + 152) == 1) then
      seed_ok = true
      print(string.format("  seed 2's world ready (made in %.1f s)", mem:read_u32(blk + 12) / 60.15))
    else
      if not seed_asked and mem:read_u16(blk + 162) == 0 then
        mem:write_u16(blk + 164, 2); mem:write_u16(blk + 166, 1); mem:write_u16(blk + 162, 11); seed_asked = true
        print(string.format("  the game started in world %d (loaded %d): asking for seed 2's", ws, mem:read_i16(blk + 152)))
      end
      return
    end
  end
  look_frame()
  if t < t_next or mem:read_u16(blk + 162) ~= 0 then return end         -- (the mailbox drained first)
  if ai <= #acts then
    local a = acts[ai]; ai = ai + 1
    a[1]()
    t_next = t + a[2]
  end
end)

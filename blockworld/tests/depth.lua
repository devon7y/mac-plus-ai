-- Blockworld world depth: a ravine (screenshots), lava hurts, sand falls, ladders climb, water
-- swims.  RAM=4M emu/run_test.sh blockworld/build/blockworld.img blockworld/tests/depth.lua 500
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
local function block(x, z, y) return mem:read_u8(rdu(244) + (x * 128 + z) * 48 + y) end
local function py() return mem:read_i32(blk + 20) / 256 end
local function put(t, x, z, y) note(function() cmd(8, x + 256 * t, z, y) end); A(function() end, 0.3) end
KEYS.w, KEYS.space = find_char("w"), find_named({"Space"})
local S = {}
local SAND, GRAVEL, ROCK, LADDER, LAVA = 8, 20, 3, 25, 21

note(function() mem:write_u16(blk + 214, 1) end)
-- a ravine: from its floor looking along it, and from its edge looking down
note(function() cmd(2, 74, 85, 256) end); A(function() end, 1.5)
look(-30)
A(function() end, 1.0)
note(function() screen:snapshot("ravine_floor.png"); check(py() < 7 and block(74, 85, 20) == 0, string.format("standing on a ravine floor (feet %.1f)", py())) end)
note(function() cmd(2, 77, 88, 512) end); A(function() end, 1.5)
look(100)
A(function() end, 1.0)
note(function() screen:snapshot("ravine_down.png"); check(block(75, 88, 10) == 0 and block(75, 88, 30) == 0, "a ravine is cut deep there") end)
look(-100)
-- lava on its floor hurts
note(function() cmd(4, 20, 20) end); A(function() end, 0.3)
note(function() check(block(75, 78, 4) == LAVA, string.format("lava on the ravine floor (block %d)", block(75, 78, 4))); cmd(2, 75, 78, 256) end); A(function() end, 1.2)
note(function() S.h = rd16(200); check(S.h < 20 and S.h > 0, string.format("standing in lava hurts (health %d)", S.h)); screen:snapshot("lava.png") end)
note(function() cmd(4, 20, 20) end); A(function() end, 0.3)
-- sand falls
note(function() cmd(2, 64, 54, 0) end); A(function() end, 1.0)
put(SAND, 66, 54, 45)
note(function() check(block(66, 54, 41) == SAND and block(66, 54, 45) == 0, string.format("sand put in the air falls to the ground (y41 %d, y45 %d)", block(66, 54, 41), block(66, 54, 45))) end)
put(GRAVEL, 66, 54, 47)
note(function() check(block(66, 54, 42) == GRAVEL, string.format("gravel lands on it (y42 %d)", block(66, 54, 42))) end)
-- a ladder against a rock pillar
note(function() cmd(2, 70, 55, 0) end); A(function() end, 1.0)
for y = 41, 45 do put(ROCK, 71, 55, y) end
for y = 41, 45 do put(LADDER, 70, 55, y) end
look(-40)
note(function() S.y = py(); screen:snapshot("ladder.png") end)
A(function() KEYS.w:set_value(1) end, 2.5); A(function() KEYS.w:clear_value() end, 0.3)
note(function() check(py() > S.y + 2, string.format("W on a ladder climbs (feet %.2f -> %.2f)", S.y, py())) end)
-- water: sinking slowly, Space swims up
note(function() cmd(2, 115, 115, 0) end); A(function() end, 3.0)
note(function() S.y = py(); print(string.format("  in the sea: feet %.2f", S.y)) end)
A(function() end, 2.0)
note(function() S.y2 = py(); check(S.y2 < S.y - 0.5, string.format("in water you sink (%.2f -> %.2f)", S.y, S.y2)); screen:snapshot("underwater.png") end)
A(function() KEYS.space:set_value(1) end, 8.0)
note(function() check(py() + 1.62 > 28, string.format("Space swims up: eye at %.2f (surface 28)", py() + 1.62)) end)
A(function() KEYS.space:clear_value() end, 0.3)
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
    if t > 300 then print("RESULT FAIL no debug block"); manager.machine:exit() end
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
  if t < t_next or mem:read_u16(blk + 162) ~= 0 then return end
  if ai <= #acts then
    local a = acts[ai]; ai = ai + 1
    a[1]()
    t_next = t + a[2]
  end
end)

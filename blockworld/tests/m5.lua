-- Blockworld M5 core-loop test: crafting in the inventory and at a workbench, tools and harvest
-- levels, a furnace smelting iron, a chest, everything saved and loaded. Items are handed over
-- through the debug block's command mailbox (cmd 1: give); everything else goes through MAME's
-- keyboard, the mouse globals and the mouse button.
--   RAM=4M emu/run_test.sh blockworld/build/blockworld.img blockworld/tests/m5.lua 900
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
local KEYS = {
  q = find_char("q"),tab = find_named({"Tab"}), e = find_char("e"), esc = find_named({"Esc"}) or find_char("`"),
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

-- 0. stand on the flat grass at the world's centre, facing +x
note(function() mem:write_u16(blk + 214, 1); mem:write_u16(blk + 258, 1) end)   -- (no natural spawning, drops come at once: mobs.lua and drops.lua test those)
note(function() mem:write_u16(blk + 164, 64); mem:write_u16(blk + 166, 64); mem:write_u16(blk + 168, 0); mem:write_u16(blk + 162, 2) end)
A(function() end, 1.0)
-- 1. given logs, rubble, coal, iron ore, two rock blocks
note(function() give(I.log, 6) end); A(function() end, 0.4)
note(function() give(I.rubble, 20) end); A(function() end, 0.4)
note(function() give(I.coal, 4) end); A(function() end, 0.4)
note(function() give(I.iron_ore, 3) end); A(function() end, 0.4)
note(function() give(I.rock, 2) end); A(function() end, 0.4)
note(function()
  local a = rdu(180)
  print(string.format("  inv at %06x, panel at (%d, %d); slots 0-3: %d x %d, %d x %d, %d x %d, %d x %d", a, rd16(176), rd16(178),
    mem:read_u8(a), mem:read_u8(a + 1), mem:read_u8(a + 4), mem:read_u8(a + 5), mem:read_u8(a + 8), mem:read_u8(a + 9),
    mem:read_u8(a + 12), mem:read_u8(a + 13)))
end)
note(function() check(count(I.log) == 6 and count(I.rubble) == 20 and count(I.coal) == 4 and count(I.iron_ore) == 3,
  "given 6 logs, 20 rubble, 4 coal, 3 iron ore") end)
-- 2. the inventory: logs -> boards (shift: all), 2 boards -> 4 sticks, 4 boards -> a workbench
note(function() if DEBUG_STOP then manager.machine:exit() end end)
key("tab", 0.6)
note(function() check(rd16(170) == 0, "Tab opens the inventory"); screen:snapshot("inventory.png") end)
pick_up(I.log); clickat(grid2, 0)
note(function() screen:snapshot("craft_boards.png") end)
shiftclick(out2)
note(function() check(count(I.boards) == 24 and count(I.log) == 0, string.format("shift-click on the output crafts all: %d boards, %d logs", count(I.boards), count(I.log))) end)
pick_up(I.boards); one_into(grid2, 0); one_into(grid2, 2); put_back(I.boards)
shiftclick(out2)
note(function() check(count(I.stick) == 4 and count(I.boards) == 22, string.format("2 boards in a column -> 4 sticks (%d sticks, %d boards)", count(I.stick), count(I.boards))) end)
pick_up(I.boards); for g = 0, 3 do one_into(grid2, g) end; put_back(I.boards)
note(function() screen:snapshot("craft_bench.png") end)
shiftclick(out2)
note(function() check(count(I.bench) == 1 and count(I.boards) == 18, string.format("4 boards -> a workbench (%d, boards %d)", count(I.bench), count(I.boards))) end)
key("tab", 0.8)
note(function() check(rd16(170) == -1, "Tab closes the inventory") end)
-- 3. place the workbench in front, open it, craft a rock pick, a furnace and a chest
select_item(I.bench)
look(80)                                           -- look down steeply: the ground 2.6 blocks ahead
key("e", 0.8)
note(function() check(rdu(104) >= 1 and count(I.bench) == 0, string.format("E places the workbench (placed %d)", rdu(104))); screen:snapshot("bench_placed.png") end)
key("e", 0.8)
note(function() check(rd16(170) == 1, "E on the workbench opens it"); screen:snapshot("bench.png") end)
pick_up(I.rubble); one_into(grid3, 0); one_into(grid3, 1); one_into(grid3, 2); put_back(I.rubble)
pick_up(I.stick); one_into(grid3, 4); one_into(grid3, 7); put_back(I.stick)
note(function() screen:snapshot("craft_pick.png") end)
shiftclick(out3)
note(function() check(count(I.rock_pick) == 1, string.format("3 rubble + 2 sticks -> a rock pick (%d)", count(I.rock_pick))) end)
pick_up(I.rubble); for g = 0, 8 do if g ~= 4 then one_into(grid3, g) end end; put_back(I.rubble)
shiftclick(out3)
note(function() check(count(I.furnace) == 1, string.format("a ring of 8 rubble -> a furnace (%d)", count(I.furnace))) end)
pick_up(I.boards); for g = 0, 8 do if g ~= 4 then one_into(grid3, g) end end; put_back(I.boards)
shiftclick(out3)
note(function() check(count(I.chest) == 1, string.format("a ring of 8 boards -> a chest (%d)", count(I.chest))) end)
key("esc", 0.8)
note(function() check(rd16(170) == -1, "Esc closes the workbench") end)
-- 4. harvest levels: rock by hand drops nothing, with the rock pick it drops rubble
select_item(I.rock)
A(function() KEYS.shift:set_value(1) end, 0.2)     -- Shift-E: against the workbench's face toward us
key("e", 0.8)
A(function() KEYS.shift:clear_value() end, 0.3)
note(function() s_rub = count(I.rubble); s_mined = rdu(100); screen:snapshot("rock_placed.png") end)
select_item(I.stick)                               -- (a stick: no pick)
A(function() BTN:set_value(1) end, 9.0)
A(function() BTN:clear_value() end, 0.5)
note(function() check(rdu(100) == s_mined + 1 and count(I.rubble) == s_rub, string.format("rock by hand: mined %d, rubble %d -> %d (none)", rdu(100) - s_mined, s_rub, count(I.rubble))) end)
select_item(I.rock)
A(function() KEYS.shift:set_value(1) end, 0.2)
key("e", 0.8)
A(function() KEYS.shift:clear_value() end, 0.3)
select_item(I.rock_pick)
note(function() s_rub = count(I.rubble); s_mined = rdu(100) end)
A(function() BTN:set_value(1) end, 1.2)
A(function() BTN:clear_value() end, 0.5)
note(function()
  local i = find(I.rock_pick); local _, _, d = slot(i)
  check(rdu(100) == s_mined + 1 and count(I.rubble) == s_rub + 1 and d == 130,
    string.format("rock with the rock pick: mined %d, rubble +%d, pick uses %d of 131", rdu(100) - s_mined, count(I.rubble) - s_rub, d)) end)
-- 5. the furnace: iron ore + coal -> an iron bar
select_item(I.furnace)
A(function() KEYS.shift:set_value(1) end, 0.2)
key("e", 0.8)
A(function() KEYS.shift:clear_value() end, 0.3)
key("e", 0.8)
note(function() check(rd16(170) == 3, "E on the furnace opens it"); screen:snapshot("furnace.png") end)
pick_up(I.iron_ore); clickat(furn_in); 
pick_up(I.coal); one_into(function() return furn_fuel() end, 0); put_back(I.coal)
A(function() end, 11.5)
note(function() screen:snapshot("furnace_done.png") end)
clickat(furn_out); put_hand_away()
note(function() check(count(I.iron_bar) == 1 and count(I.coal) == 3, string.format("10 s later: %d iron bar, coal %d", count(I.iron_bar), count(I.coal))) end)
key("esc", 0.8)
-- 6. the chest: shift-click the iron bar in, close, open again
select_item(I.chest)
A(function() KEYS.shift:set_value(1) end, 0.2)
key("e", 0.8)
A(function() KEYS.shift:clear_value() end, 0.3)
key("e", 0.8)
note(function() check(rd16(170) == 2, "E on the chest opens it") end)
A(function() local i = find(I.iron_bar); s_bar = i end, 0.1)
A(function() KEYS.shift:set_value(1); mouse_to(inv_pos(s_bar)) end, 0.2)
A(function() BTN:set_value(1) end, 0.15); A(function() BTN:clear_value() end, 0.2)
A(function() KEYS.shift:clear_value() end, 0.3)
note(function() check(count(I.iron_bar) == 0, "shift-click moves the iron bar into the chest"); screen:snapshot("chest.png") end)
key("esc", 0.8)
-- 7. save and reload: the chest and the inventory come back
note(function() s_inv = {}; for i = 0, 35 do local it, c = slot(i); s_inv[i] = it * 256 + c end end)
key("esc", 1.0)
key("q", 0.8)
note(function() blk = nil; next_scan = manager.machine.time:as_double() + 8; reloading = true; rescan_t = manager.machine.time:as_double() end)
note(function()
  local same = true
  for i = 0, 35 do local it, c = slot(i); if s_inv[i] ~= it * 256 + c then same = false end end
  check(rd16(152) == 1, string.format("restarted from the save (load error %d, save error %d)", rd16(154), rd16(156)))
  check(same, "the inventory came back")
  local ch, found = rdu(188), false
  for k = 0, 47 do
    local a = ch + k * 116
    if mem:read_u16(a + 6) ~= 0 then
      for q = 0, 26 do if mem:read_u8(a + 8 + 4 * q) == I.iron_bar then found = true end end
    end
  end
  check(found, "the chest still holds the iron bar")
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
    if t > 300 and not reloading then print("RESULT FAIL no debug block"); manager.machine:exit() end
    return
  end
  if reloading and rd16(152) ~= 1 then                     -- (an old copy of the block: look again)
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
  if t < t_next then return end
  if ai <= #acts then
    local a = acts[ai]; ai = ai + 1
    a[1]()
    t_next = t + a[2]
  end
end)

local seed_ok, seed_asked
-- Blockworld showcase: a scripted tour on the 4 MB Plus at real speed, one screenshot per scene
-- (RUN_DIR/shots/sc_NN.png, labels in RUN_DIR/labels.txt; tests/showcase_sheet.py makes the
-- contact sheet). Scenes are staged with the debug block where needed (teleports, spawns, blocks).
--   SLOW=1.5 RAM=4M emu/run_test.sh blockworld/build/blockworld.img blockworld/tests/showcase.lua 1500
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
  shift = find_named({"Shift", "Left Shift"}), one = find_char("1"), two = find_char("2"), three = find_char("3"),
  v = find_char("v"), w = find_char("w"), h = find_char("h")}
for k, f in pairs(KEYS) do if not f then print("RESULT FAIL (no key " .. k .. ")"); manager.machine:exit() end end

-- items (gen_items.py): blocks keep their world.h numbers
local I = {bed = 19, seeds = 41, torch = 29, gem = 36, iron_pick = 74, hoe = 68, log = 5, boards = 6, rubble = 4, rock = 3, coal_ore = 10, iron_ore = 11, bench = 14, chest = 17,
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
local SC = 1.6                                     -- (waits stretched for SLOW=1.5)
local function A(f, secs) acts[#acts + 1] = {f, (secs or 0.25) * SC} end
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
local function look(dy)
  A(function()
    local v, h = mem:read_i16(0x828), mem:read_i16(0x82A)
    print(string.format("  look %d: pitch %d, MTemp (%d, %d) Mouse (%d, %d)", dy, rd16(30), v, h, mem:read_i16(0x830), mem:read_i16(0x832)))
    look_dy, look_p0, look_n, look_tk, look_loops = dy, rd16(30), 0, rdu(56), 0
    look_write(dy)
  end, 1.0)
  A(function() look_dy = nil; print(string.format("  -> pitch %d, MTemp (%d, %d)", rd16(30), mem:read_i16(0x828), mem:read_i16(0x82A))) end, 0.1)
end


local function cmd(c, a0, a1, a2) mem:write_u16(blk + 164, a0 or 0); mem:write_u16(blk + 166, a1 or 0); mem:write_u16(blk + 168, a2 or 0); mem:write_u16(blk + 162, c) end
local function C(c, a0, a1, a2, secs) note(function() cmd(c, a0, a1, a2) end); A(function() end, secs or 0.4) end
local function put(t, x, z, y) C(8, x + 256 * t, z, y, 0.2) end
local labels = io.open((os.getenv("RUN_DIR") or ".") .. "/labels.txt", "w")
local nshot = 0
local function snap(label)
  nshot = nshot + 1
  local f = string.format("sc_%02d.png", nshot)
  screen:snapshot(f)
  labels:write(f .. "\t" .. label .. "\n"); labels:flush()
  print(string.format("  shot %d: %s (t=%.0f)", nshot, label, manager.machine.time:as_double()))
end
local function shot(label) note(function() snap(label) end) end
local waiting
local function W(pred, secs) acts[#acts + 1] = {function() waiting = {pred, manager.machine.time:as_double() + secs} end, 0} end
local function mob(i) local a = rdu(216) + 36 * i
  return mem:read_u8(a + 20), mem:read_u8(a + 21), mem:read_i32(a + 4) / 256 end
local function heal() C(4, 20, 20) end

note(function() mem:write_u16(blk + 214, 1) end)                    -- natural spawning off: scenes are staged
local function P(p) C(9, p & 0xFFFF, 0, 0, 0.6) end                 -- pitch (negative: down)
local function PY(p, y) C(9, p & 0xFFFF, y, 1, 0.6) end
-- landscapes
C(2, 84, 26, 960, 2.0); P(-12)
A(function() end, 2.0)
shot("A daytime landscape: trees on terraced hills")
C(2, 70, 96, 0, 2.0); P(0)
A(function() end, 2.0)
shot("Hills and a ravine")
-- mining a block, the hotbar
C(2, 54, 64, 0, 2.0); P(-160)
A(function() end, 1.0)
A(function() BTN:set_value(1) end, 0.5)
shot("Mining a block (the bar under the crosshair)")
A(function() end, 0.6)
A(function() BTN:clear_value() end, 1.0)
P(-60)
A(function() end, 1.0)
shot("Mined: the soil lies on the ground")
A(function() KEYS.w:set_value(1) end, 0.4); A(function() KEYS.w:clear_value() end, 1.5)
shot("Walking over it picks it up: soil in the hotbar")
P(-160)                                                               -- (the ground in reach again)
-- the inventory: 2x2 crafting
note(function() give(I.log, 4) end); A(function() end, 0.4)
key("tab", 1.0)
pick_up(I.log); clickat(grid2, 0)
A(function() end, 0.5)
shot("Tab: the inventory, logs in the 2x2 grid make boards")
shiftclick(out2)
key("tab", 1.0)
-- the workbench: a pickaxe
note(function() give(I.bench, 1) end); A(function() end, 0.4)
note(function() give(I.stick, 4) end); A(function() end, 0.4)
select_item(I.bench)
key("e", 1.0)
key("e", 1.0)
pick_up(I.boards); one_into(grid3, 0); one_into(grid3, 1); one_into(grid3, 2); put_back(I.boards)
pick_up(I.stick); one_into(grid3, 4); one_into(grid3, 7); put_back(I.stick)
A(function() end, 0.5)
shot("The workbench: 3 boards and 2 sticks make a wood pick")
shiftclick(out3)
key("esc", 1.0)
-- the furnace
note(function() give(I.furnace, 1) end); A(function() end, 0.4)
note(function() give(I.iron_ore, 4) end); A(function() end, 0.4)
note(function() give(I.coal, 4) end); A(function() end, 0.4)
select_item(I.furnace)
A(function() KEYS.shift:set_value(1) end, 0.2); key("e", 1.0); A(function() KEYS.shift:clear_value() end, 0.3)
key("e", 1.0)
pick_up(I.iron_ore); clickat(furn_in)
pick_up(I.coal); one_into(function() return furn_fuel() end, 0); put_back(I.coal)
A(function() end, 7.0)
shot("The furnace smelting iron ore with coal (flame, progress, a bar out)")
key("esc", 1.0)
-- a chest
note(function() give(I.chest, 1) end); A(function() end, 0.4)
select_item(I.chest)
A(function() KEYS.shift:set_value(1) end, 0.2); key("e", 1.0); A(function() KEYS.shift:clear_value() end, 0.3)
key("e", 1.0)
for _, it in ipairs({I.boards, I.coal, I.stick}) do
  A(function() s_i = find(it) end, 0.1)
  A(function() if s_i then KEYS.shift:set_value(1); mouse_to(inv_pos(s_i)) end end, 0.2)
  A(function() BTN:set_value(1) end, 0.15); A(function() BTN:clear_value() end, 0.2)
  A(function() KEYS.shift:clear_value() end, 0.3)
end
shot("A chest (27 slots); Shift-click moves stacks across")
key("esc", 1.0)
-- caves: unlit, then lit by a torch; ore veins; an iron pick on gem ore
C(2, 51, 53, 0, 1.0); C(5, 8, 0, 0, 2.0); P(0)
A(function() end, 1.5)
shot("A cave 32 blocks down at noon, no torch: dark")
note(function() give(I.torch, 8) end); A(function() end, 0.4)
select_item(I.torch)
P(-220)                                                      -- (the floor 4 blocks along the ray: in reach)
key("e", 1.0)
P(0)
A(function() end, 1.5)
shot("The same cave lit by a torch")
for _, b in ipairs({{16, 56, 53, 7}, {16, 57, 53, 8}, {16, 57, 53, 9}, {15, 56, 52, 8}, {15, 57, 54, 9}, {11, 56, 54, 8}, {10, 57, 52, 9}, {11, 57, 52, 10}}) do
  put(b[1], b[2], b[3], b[4])
end
C(2, 53, 53, 0, 1.0); C(5, 8, 0, 0, 1.5); P(-200)
key("e", 1.0)                                                         -- a torch on the floor near the wall
C(2, 52, 53, 0, 1.0); C(5, 8, 0, 0, 1.5); P(-20)
A(function() end, 1.5)
shot("Ore veins in torchlight: coal, iron, gold, gem")
note(function() give(I.iron_pick, 1) end); A(function() end, 0.4)
select_item(I.iron_pick)
C(2, 54, 53, 0, 1.0); C(5, 8, 0, 0, 1.5); P(-80)
A(function() end, 1.0)
note(function() s_gem = count(I.gem); print(string.format("  aiming at block (%d, %d, %d), pitch %d", rd16(90), rd16(92), rd16(94), rd16(30))) end)
A(function() BTN:set_value(1) end, 1.0)
shot("An iron pick mining gem ore")
A(function() end, 6.0)
A(function() BTN:clear_value() end, 1.0)
note(function() print(string.format("  gems %d -> %d", s_gem, count(I.gem))) end)
-- animals in a field
C(7)
C(2, 64, 54, 960); C(6, 0, 6, 256)
C(2, 64, 54, 0); C(6, 1, 4, 768)
C(2, 64, 54, 60); C(6, 2, 7, 256)
C(2, 64, 54, 30); C(6, 3, 3, 768)
C(2, 64, 54, 990); C(6, 0, 10, 512)
C(2, 64, 54, 0, 3.0)
shot("Animals in a field: yaks, a boar, a ram, a fowl")
-- a crawler on a wall
C(7)
for x = 63, 65 do for z = 61, 63 do for y = 41, 44 do put(3, x, z, y) end end end   -- a tower to stand on
C(2, 64, 62, 0, 1.5); P(-150)
C(6, 6, 6, 512, 0.2)
W(function() local k, al, y = mob(0); return al == 1 and y >= 44.4 end, 20)
note(function() snap("A crawler climbing the tower the player stands on") end)
C(7)
-- a sizzler's crater
note(function() s_boom = rd16(238) end)
C(2, 64, 54, 0, 1.5)
C(6, 7, 4, 512, 0.2)
W(function() return rd16(238) > s_boom end, 15)
A(function() end, 0.5)
P(-60)
A(function() end, 2.0)
shot("A sizzler went off: its crater")
heal()
C(7)
-- night: a shambler and an archer
heal()
C(3, 1500, 0, 0, 1.5)
C(2, 58, 67, 1000); C(6, 4, 8, 512)
C(2, 58, 67, 40); C(6, 5, 11, 512)
C(2, 58, 67, 0, 0.6); P(0)
shot("Night: a shambler and an archer coming")
C(7)
heal()
-- a bed: sleeping through the night
note(function() give(I.bed, 1) end); A(function() end, 0.4)
select_item(I.bed)
C(2, 60, 47, 0, 1.5); P(-160)
key("e", 1.0)
key("e", 1.0)
P(-40)
A(function() end, 1.0)
shot("E on the bed at night: you sleep until morning")
-- farming: tilled ground and wheat in four stages, across the view
C(2, 54, 66, 0, 1.0)
note(function()
  local a, k, st = rdu(196), 0, {17, 19, 21, 23}
  for i = 0, 255 do
    if k < 4 and mem:read_u8(a + i * 8 + 6) == 0 then
      k = k + 1
      mem:write_u16(a + i * 8, 57); mem:write_u16(a + i * 8 + 2, 41); mem:write_u16(a + i * 8 + 4, 64 + k)
      mem:write_u8(a + i * 8 + 7, st[k]); mem:write_u8(a + i * 8 + 6, 2)
    end
  end
end)
for z = 65, 68 do put(27, 57, z, 40) end
P(-40)
A(function() end, 2.0)
shot("Farming: farmland and wheat in four stages")
-- death
P(0)
C(4, 1, 20, 0, 0.3)
C(5, 50, 0, 0, 0.2)
W(function() return rd16(200) == 20 end, 15)
A(function() end, 1.5)
shot("A deadly fall: back at the spawn, things in a chest")
-- the Esc menu
key("esc", 1.5)
shot("Esc: the menu (resume, view distance, how to play, save and quit)")
key("h", 2.0)
shot("H: how to play (the controls and the first steps)")
key("v", 2.0)
key("esc", 1.5)
note(function() labels:close(); print("RESULT done"); manager.machine:exit() end)

local t_next, titled = 0, false
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not blk then
    if t >= next_scan then
      blk = scan(); next_scan = t + 4
      if blk then print(string.format("BWGM at %06x (t=%.1f)", blk, t)) end
    end
    if t > 600 then print("RESULT FAIL no debug block"); manager.machine:exit() end
    return
  end
  if not titled and (rdu(8) == 1 or rdu(8) == 2) then
    title_t = title_t or t
    if rdu(8) == 2 or t > title_t + 8 then
      titled = true
      snap(rdu(8) == 1 and "Starting: a new world is made (about 45 s on a 4 MB Plus)" or "The first frame of a new world")
    end
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
  if waiting then
    if waiting[1]() or t > waiting[2] then waiting = nil; t_next = t else return end
  end
  if t < t_next or mem:read_u16(blk + 162) ~= 0 then return end
  if ai <= #acts then
    local a = acts[ai]; ai = ai + 1
    a[1]()
    t_next = t + a[2]
  end
end)

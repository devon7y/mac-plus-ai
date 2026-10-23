-- Blockworld looking straight down and up: dig down through the block under the feet (twice),
-- mine the block over the head, place one there, and pillar up (jump, place under the feet);
-- screenshots looking down and up at each view size. SIZE=n starts at that view size (default 2).
--   RAM=4M emu/run_test.sh blockworld/build/blockworld.img blockworld/tests/look.lua 600
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
local BTN = (ports[":macadb:MOUSE0"] and ports[":macadb:MOUSE0"].fields["Mouse Button 0"])   -- (the SE's ADB mouse has
  or (ports[":MOUSE0"] and ports[":MOUSE0"].fields["Mouse Button"])        --  buttons 0-2; the Mac reads 0)
local KEYS = {tab = find_named({"Tab"}), e = find_char("e"), esc = find_named({"Esc"}) or find_char("`"),
  shift = find_named({"Shift", "Left Shift"}), one = find_char("1"), two = find_char("2"), three = find_char("3")}
for k, f in pairs(KEYS) do if not f then print("  (no key " .. k .. " on this keyboard)") end end

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
  if look_dy and rd16(270) ~= look_p0 then look_dy = nil end          --  aim is the same: its copy lags)
  if look_dy and rdu(56) ~= look_tk then look_tk = rdu(56); look_loops = look_loops + 1 end
  if look_dy and look_n < 30 and look_loops >= 2 and mem:read_i16(0x828) == 171 then
    look_n = look_n + 1; look_loops = 0; look_write(look_dy)
  end
end
local function look(dy)
  A(function()
    local v, h = mem:read_i16(0x828), mem:read_i16(0x82A)
    print(string.format("  look %d: aim %d, MTemp (%d, %d) Mouse (%d, %d)", dy, rd16(30), v, h, mem:read_i16(0x830), mem:read_i16(0x832)))
    look_dy, look_p0, look_n, look_tk, look_loops = dy, rd16(270), 0, rdu(56), 0
    look_write(dy)
  end, 1.0)
  A(function() look_dy = nil; print(string.format("  -> pitch %d, MTemp (%d, %d)", rd16(30), mem:read_i16(0x828), mem:read_i16(0x82A))) end, 0.1)
end

local function cmd(c, a0, a1, a2) mem:write_u16(blk + 164, a0 or 0); mem:write_u16(blk + 166, a1 or 0); mem:write_u16(blk + 168, a2 or 0); mem:write_u16(blk + 162, c) end
local function C(c, a0, a1, a2, secs) note(function() cmd(c, a0, a1, a2) end); A(function() end, secs or 0.5) end
KEYS.space, KEYS.e = find_named({"Space"}), find_char("e")
local SIZE = tonumber(os.getenv("SIZE") or "2")
local function rd(o) return mem:read_i32(blk + o) end
local function block(x, y, z) return mem:read_u8(rdu(244) + (x * 128 + z) * 48 + y) end
local function feet() return math.floor(rd(16) / 256), math.floor(rd(20) / 256), math.floor(rd(24) / 256) end
local labels = io.open((os.getenv("RUN_DIR") or ".") .. "/labels.txt", "w")
local function snap(f, label)
  screen:snapshot(f); print("  shot " .. f)
  if label then labels:write(f .. "\t" .. label .. "\n"); labels:flush() end
end
local function aim_to(a)            -- the mouse, 80 px at a time (1 px = 2/1024 turn)
  for _ = 1, 8 do
    A(function() local d = rd16(270) - a; if d ~= 0 then
      local s = d // 2                -- (2 units a pixel)
      s = s > 80 and 80 or (s < -80 and -80 or s)
      if s == 0 then s = d end
      look_dy, look_p0, look_n, look_tk, look_loops = s, rd16(270), 0, rdu(56), 0; look_write(s) end end, 0.9)
  end
  A(function() look_dy = nil end, 0.1)
end
local function mine_until(f, what, secs)   -- hold the button until f() (at most secs)
  local t_end
  A(function() BTN:set_value(1); t_end = manager.machine.time:as_double() + secs end, 0.2)
  for _ = 1, math.floor(secs / 0.5) do
    A(function() if f() or manager.machine.time:as_double() > t_end then BTN:clear_value() end end, 0.5)
  end
  A(function() BTN:clear_value(); check(f(), what) end, 1.5)
end
local S = {}
note(function()
  mem:write_u16(blk + 214, 1); mem:write_u16(blk + 258, 1)
  print(string.format("  machine %02x", rd16(272)))
end)
C(10, SIZE, 10, 0, 2.0)
C(3, 600, 0, 0, 0.5)                    -- noon
C(2, 64, 64, 0, 2.0)
C(1, 69, 1, 0, 0.5)                     -- a rock pick (slot 1), boards (slot 2)
C(1, 6, 20, 0, 0.5)
note(function() S.x, S.y, S.z = feet(); print(string.format("  feet block (%d, %d, %d), under it %d", S.x, S.y, S.z, block(S.x, S.y - 1, S.z))) end)
select_item(69)
-- straight down
aim_to(-240)
A(function()
  check(rd16(270) == -240, string.format("the mouse looks straight down: aim %d (-240 = 84 degrees)", rd16(270)))
  check(rd16(88) == 1 and rd16(90) == S.x and rd16(92) == S.y - 1 and rd16(94) == S.z,
    string.format("the crosshair picks the block under the feet: hit %d (%d, %d, %d), face %d", rd16(88), rd16(90), rd16(92), rd16(94), rd16(96)))
  snap("look_down.png", "Looking straight down (the mouse, 84 degrees): the block under the feet is selected")
end, 0.5)
mine_until(function() return block(S.x, S.y - 1, S.z) == 0 end, "mined the block under the feet", 12)
A(function() local _, y = feet(); check(y == S.y - 1, string.format("fell into the hole: feet at %d (was %d)", y, S.y))
  check(rd16(92) == S.y - 2, string.format("the next block down is picked: hit y %d", rd16(92))) end, 1.0)
mine_until(function() return block(S.x, S.y - 2, S.z) == 0 end, "mined the next one: two deep", 12)
A(function() local _, y = feet(); check(y == S.y - 2, string.format("two blocks down: feet at %d", y)); snap("dig_down.png", "Two blocks dug straight down; the next one is the target, below the view: the marker shows it") end, 1.5)
-- straight up: a ceiling over the head (cmd 8: boards at the surface level)
note(function() cmd(8, S.x + 256 * 6, S.z, S.y) end)
A(function() end, 1.0)
aim_to(240)
A(function()
  check(rd16(270) == 240, string.format("the mouse looks straight up: aim %d", rd16(270)))
  check(rd16(88) == 1 and rd16(90) == S.x and rd16(92) == S.y and rd16(94) == S.z,
    string.format("the crosshair picks the block over the head: hit %d (%d, %d, %d)", rd16(88), rd16(90), rd16(92), rd16(94)))
  snap("look_up.png", "Looking straight up from the hole: the block over the head fills the view, selected")
end, 0.5)
mine_until(function() return block(S.x, S.y, S.z) == 0 end, "mined the block over the head", 12)
note(function() cmd(8, S.x + 256 * 6, S.z, S.y + 1) end)   -- one higher: something to place against
A(function() end, 1.0)
select_item(6)
A(function() S.placed = rdu(104); check(rd16(88) == 1 and rd16(92) == S.y + 1, string.format("picks the block two over the head: y %d", rd16(92))) end, 0.3)
key("e")
A(function() check(block(S.x, S.y, S.z) == 6 and rdu(104) == S.placed + 1,
  string.format("placed boards over the head: block %d, placed %d -> %d", block(S.x, S.y, S.z), S.placed, rdu(104))); snap("placed_up.png", "Mined it, and placed boards over the head (against the block above)") end, 1.0)
-- pillar: clear the ceiling, look down, jump, place under the feet at the top of the jump
note(function() cmd(8, S.x, S.z, S.y) end)
A(function() end, 0.5)
note(function() cmd(8, S.x, S.z, S.y + 1) end)
A(function() end, 0.5)
aim_to(-240)
A(function() S.py = rd(20); S.placed = rdu(104) end, 0.2)
for _ = 1, 4 do
  A(function() KEYS.space:set_value(1) end, 0.25)
  A(function() KEYS.space:clear_value(); KEYS.e:set_value(1) end, 0.3)
  A(function() KEYS.e:clear_value() end, 1.2)
end
A(function() local _, y = feet()
  check(rdu(104) >= S.placed + 1 and y >= S.y - 1, string.format("jumping and placing under the feet climbs: placed %d, feet %d -> %d",
    rdu(104) - S.placed, S.y - 2, y)); snap("pillar.png", "Jump and place under the feet: a pillar of boards, looking down at its top") end, 1.0)
-- the other sizes: looking down and up on open ground
C(2, 60, 64, 0, 2.0)
for _, sz in ipairs({0, 1, 2}) do
  C(10, sz, 10, 0, 2.0)
  aim_to(-240)
  local nm = ({"Normal", "Large", "Full"})[sz + 1]
  A(function() snap(string.format("down_%d.png", sz), nm .. ": looking straight down on open ground") end, 0.5)
  aim_to(240)
  A(function() snap(string.format("up_%d.png", sz), nm .. ": looking straight up (nothing in reach: the arrow)") end, 0.5)
  aim_to(-120)
  A(function() snap(string.format("down45_%d.png", sz), nm .. ": looking 42 degrees down") end, 0.5)
end
note(function() print(string.format("RESULT %s (%d failed checks)", fails == 0 and "PASS" or "FAIL", fails)); manager.machine:exit() end)

local t_next = 0
local seed_ok, seed_asked
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not blk then
    if t >= next_scan then blk = scan(); next_scan = t + 5 end
    if t > 400 then print("RESULT FAIL no debug block"); manager.machine:exit() end
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
  if ai <= #acts then local a = acts[ai]; ai = ai + 1; a[1](); t_next = t + a[2] end
end)

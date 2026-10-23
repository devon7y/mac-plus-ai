-- Blockworld frame rate by view size (SLOW=1.5 Plus, SLOW=1.3 SE): for each size and view distance,
-- 12 s walking forward and back on the plateau (frames drawn when the view changes; the turns
-- stop the view for a moment, and a refinement is drawn), then 12 s walking in a circle.
--   SLOW=1.5 RAM=4M emu/run_test.sh blockworld/build/blockworld.img blockworld/tests/sizefps.lua 900
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
local BTN
BTN = (ports[":macadb:MOUSE0"] and ports[":macadb:MOUSE0"].fields["Mouse Button 0"])   -- (the SE's ADB mouse has
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
local function C(c, a0, a1, a2, secs) note(function() cmd(c, a0, a1, a2) end); A(function() end, secs or 0.5) end
KEYS.w, KEYS.s = find_char("w"), find_char("s")
local walking, circling, M = false, false, {}
KEYS.right = find_named({"Cursor Right", "Right Arrow"})
note(function() mem:write_u16(blk + 214, 1); print(string.format("  machine %02x, full width %d", rd16(272), rd16(266))) end)
for _, sz in ipairs({0, 1, 2}) do
  for _, dist in ipairs({10, 8}) do
    C(10, sz, dist, 0, 2.0)
    C(2, 64, 64, 0, 2.0)
    note(function() M.f, M.t, M.mt, M.mc = rdu(32), manager.machine.time:as_double(), rdu(48), rdu(52); walking = true end)
    A(function() end, 6)
    note(function() screen:snapshot(string.format("walk_%d_%d.png", rd16(264), rd16(76))) end)   -- (a moving frame)
    A(function() end, 6)
    note(function()
      walking = false
      local dt = manager.machine.time:as_double() - M.t
      local fr, mc = rdu(32) - M.f, rdu(52) - M.mc
      local ms = (rdu(48) - M.mt) / 60.15 / math.max(mc, 1) * 1000
      print(string.format("RESULT size %d (%dx%d) view %d: walking %.2f frames/s (moving frames %d, render+present %.0f ms = %.2f fps)",
        rd16(264), rd16(266), rd16(268), rd16(76), fr / dt, mc, ms, 1000 / ms))
      print(string.format("  (back and forth: %d other frames, refinements at the turns)", fr - mc))
      screen:snapshot(string.format("size_%d_%d.png", rd16(264), rd16(76)))
    end)
    A(function() end, 1.5)
    note(function() print(string.format("RESULT size %d view %d: a refinement (standing still) %.0f ms", rd16(264), rd16(76), rdu(84) / 60.15 * 1000)) end)
    -- walking in a circle (W and the right arrow held): the view changes every frame
    C(2, 64, 64, 0, 1.5)
    note(function() M.f, M.t, M.rf = rdu(32), manager.machine.time:as_double(), rdu(40); circling = true end)
    A(function() end, 12)
    note(function()
      circling = false
      local dt = manager.machine.time:as_double() - M.t
      print(string.format("RESULT size %d view %d: circling %.2f frames/s (refinements %d)", rd16(264), rd16(76), (rdu(32) - M.f) / dt, rdu(40) - M.rf))
    end)
    A(function() end, 1.0)
  end
end
note(function() print("RESULT done"); manager.machine:exit() end)

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
  if walking then
    local fwd = math.floor(t / 0.4) % 2 == 0
    KEYS.w:set_value(fwd and 1 or 0); KEYS.s:set_value(fwd and 0 or 1)
  elseif circling then
    KEYS.w:set_value(1); KEYS.s:set_value(0); KEYS.right:set_value(1)
  elseif KEYS.w then KEYS.w:set_value(0); KEYS.s:set_value(0); KEYS.right:set_value(0) end
  if t < t_next or mem:read_u16(blk + 162) ~= 0 then return end
  if ai <= #acts then local a = acts[ai]; ai = ai + 1; a[1](); t_next = t + a[2] end
end)

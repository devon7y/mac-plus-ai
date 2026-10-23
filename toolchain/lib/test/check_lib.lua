-- Check toolchain/lib's test app (LibTest) in MAME, on the Plus or the SE:
--   CHECK_AT=30 emu/run_test.sh toolchain/build/libtest_narrow.img toolchain/lib/test/check_lib.lua 200
--   MACHINE=macsefd RAM=4M HARDDISK=toolchain/build/libtest_se7.hd CHECK_AT=60 emu/run_test.sh none toolchain/lib/test/check_lib.lua 300
-- Compares the view on screen with the offscreen buffer byte for byte, prints the speed and
-- pacing numbers, then presses every mapped key and checks the app's PlInput.held.
mem = manager.machine.devices[":maincpu"].spaces["program"]
screen = manager.machine.screens[":screen"]
local ports = manager.machine.ioport.ports
local SE = ports[":macadb:KEY0"] ~= nil
local MAGIC = 0x504C4942                 -- 'PLIB'
local CHECK_AT = tonumber(os.getenv("CHECK_AT") or "30")
local fails = 0
local function check(ok, what)
  print(string.format("CHECK %-4s %s", ok and "ok" or "FAIL", what))
  if not ok then fails = fails + 1 end
end
local function ram_top()
  local n, u = manager.options.entries["ramsize"]:value():match("(%d+)(%a?)")
  return tonumber(n) * ((u == "M" or u == "m") and 1048576 or (u == "K" or u == "k") and 1024 or 1)
end
local function scan()
  for a = ram_top() - 8, 0x1000, -2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then return a end
  end
end

-- {port, field, bit name, bit}
local keys = SE and {
  {":macadb:KEY3", "Up Arrow", "UP", 0x1}, {":macadb:KEY3", "Down Arrow", "DOWN", 0x2},
  {":macadb:KEY3", "Left Arrow", "LEFT", 0x4}, {":macadb:KEY3", "Right Arrow", "RIGHT", 0x8},
  {":macadb:KEY0", "x  X", "A (X)", 0x10}, {":macadb:KEY0", "z  Z", "B (Z)", 0x20},
  {":macadb:KEY2", "Return", "START (Return)", 0x40}, {":macadb:KEY3", "Tab", "SELECT (Tab)", 0x80},
  {":macadb:KEY3", "Space", "SPECIAL (Space)", 0x100}, {":macadb:KEY3", "Esc", "ESC", 0x400},
} or {
  {":kbd:usp:ROW3", "Cursor Up", "UP", 0x1}, {":kbd:usp:ROW4", "Cursor Down", "DOWN", 0x2},
  {":kbd:usp:ROW2", "Cursor Left", "LEFT", 0x4}, {":kbd:usp:ROW3", "Cursor Right", "RIGHT", 0x8},
  {":kbd:usp:ROW8", "x  X", "A (X)", 0x10}, {":kbd:usp:ROW8", "z  Z", "B (Z)", 0x20},
  {":kbd:usp:ROW4", "Return", "START (Return)", 0x40}, {":kbd:usp:ROW8", "Tab", "SELECT (Tab)", 0x80},
  {":kbd:usp:ROW8", "Space", "SPECIAL (Space)", 0x100},
}
local cmd = SE and {":macadb:KEY3", "Command / Open Apple"} or {":kbd:usp:P2", "Command"}
local qkey = SE and {":macadb:KEY0", "q  Q"} or {":kbd:usp:ROW9", "q  Q"}
local function field(k) return ports[k[1]].fields[k[2]] end

local blk, phase, t_next, ki = nil, "scan", CHECK_AT, 1
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if t < t_next then return end
  if phase == "scan" then
    blk = scan()
    if blk and mem:read_u32(blk + 12) >= 1 then
      check(mem:read_u32(blk + 12) == 1, "found the app while it shows the test pattern (state 1)")
      print(string.format("'PLIB' block at %06x (t=%.1f) on %s", blk, t, SE and "macsefd" or "macplus"))
      local buf, base = mem:read_u32(blk + 24), mem:read_u32(blk + 28)
      local srb, w, h, drb = mem:read_i16(blk + 40), mem:read_i16(blk + 42), mem:read_i16(blk + 44), mem:read_i16(blk + 46)
      print(string.format("view at screen %06x rect=(t%d,l%d,b%d,r%d) %dx%d, buffer %06x, rowBytes src %d dst %d",
        base, mem:read_i16(blk + 32), mem:read_i16(blk + 34), mem:read_i16(blk + 36), mem:read_i16(blk + 38), w, h, buf, srb, drb))
      check((mem:read_u32(blk + 16) & 1) == 1, "pl_view_init")
      local bad = 0
      for y = 0, h - 1 do
        for x = 0, w // 8 - 1 do
          if mem:read_u8(base + y * drb + x) ~= mem:read_u8(buf + y * srb + x) then bad = bad + 1 end
        end
      end
      check(bad == 0, string.format("pl_present: screen == buffer for all %d bytes (%d differ)", h * w // 8, bad))
      screen:snapshot("libtest_pattern.png")
      mem:write_u32(blk + 92, 1)          -- ack: the app moves on to the speed tests
      phase = "speed"; t_next = t + 1
    elseif t > CHECK_AT + 150 then
      check(false, "block found (TIMEOUT)"); phase = "done"
    else
      t_next = t + 5
    end
  elseif phase == "speed" then
    if mem:read_u32(blk + 12) < 3 then t_next = t + 1; return end
    local p, c = mem:read_u32(blk + 48), mem:read_u32(blk + 52)
    print(string.format("60 x pl_present: %d ticks = %.1f ms each; 60 x CopyBits: %d ticks = %.1f ms each",
      p, p / 60 / 60.15 * 1000, c, c / 60 / 60.15 * 1000))
    print(string.format("pacing 30 Hz, 60 frames: idle %d ticks (%d late), presenting each frame %d ticks (%d late)",
      mem:read_u32(blk + 56), mem:read_u32(blk + 60), mem:read_u32(blk + 64), mem:read_u32(blk + 68)))
    check((mem:read_u32(blk + 16) & 2) == 2, "pacing idle: 60 frames in 119-121 ticks, none late")
    check((mem:read_u32(blk + 16) & 4) == 4, "pacing with a present per frame: none late")
    phase = "keys"; t_next = t + 1
  elseif phase == "keys" then
    if mem:read_u32(blk + 12) < 4 then t_next = t + 1; return end
    -- Press, then wait until the app has run 6 more frames before reading PlInput.held (under
    -- System 7 the app's GetNextEvent can hand the Finder a long time slice; see README).
    local k = keys[ki]
    local frames = mem:read_u32(blk + 84)
    if not k.down then
      field(k):set_value(1); k.down = true; k.f0 = frames; k.t0 = t; t_next = t + 0.1
    elseif not k.checked then
      if frames < k.f0 + 6 and t < k.t0 + 5 then t_next = t + 0.1; return end
      if t - k.t0 > 0.5 then print(string.format("note: app stalled, %d frames in %.1f s after pressing %s", frames - k.f0, t - k.t0, k[3])) end
      local held = mem:read_u32(blk + 72)
      check(held & k[4] ~= 0 and held & ~k[4] & 0x7FF == 0, string.format("key %-16s -> held=%03x (frame %d)", k[3], held, frames))
      field(k):clear_value(); k.checked = true; k.f1 = frames; t_next = t + 0.1
    elseif frames < k.f1 + 6 and t < k.t0 + 10 then
      t_next = t + 0.1                    -- let the release register too
    else
      ki = ki + 1
      if ki > #keys then phase = "cmdq" end
    end
  elseif phase == "cmdq" then
    field(cmd):set_value(1); field(qkey):set_value(1); phase = "cmdq2"; t_next = t + 0.3
  elseif phase == "cmdq2" then
    local held = mem:read_u32(blk + 72)
    check(held & 0x200 ~= 0, string.format("Command-Q       -> held=%03x (PL_QUIT)", held))
    field(cmd):clear_value(); field(qkey):clear_value()
    -- hold Right for 1 s to move the square, then screenshot
    field(keys[4]):set_value(1); phase = "move"; t_next = t + 1.0
    sx0 = mem:read_i16(blk + 88)
  elseif phase == "move" then
    field(keys[4]):clear_value()
    local sx = mem:read_i16(blk + 88)
    check(sx > sx0, string.format("square moved right: x %d -> %d", sx0, sx))
    print(string.format("frames %d, presses %d", mem:read_u32(blk + 84), mem:read_u32(blk + 80)))
    screen:snapshot("libtest_input.png")
    phase = "done"
  end
  if phase == "done" then
    print(fails == 0 and "RESULT PASS" or string.format("RESULT FAIL (%d failures)", fails))
    phase = "exit"; manager.machine:exit()
  end
end)

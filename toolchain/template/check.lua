-- Check the template app ("Hello, Plus") inside MAME. Run with emu/run_test.sh:
--   emu/run_test.sh build/hello_disk.img check.lua 90            (1 MB)
--   RAM=4M CHECK_AT=70 emu/run_test.sh build/hello_disk.img check.lua 120
--   SE / System 7 (see toolchain/README.md):
--   MACHINE=macsefd RAM=4M HARDDISK=build/hello_se7.hd EXPECT_W=512 CHECK_AT=70 emu/run_test.sh none check.lua 250
-- Environment:
--   EXPECT_W  expected screenBits width (448 with Narrow Screen at 32/32, 512 without; default 448)
--   CHECK_AT  emulated second at which to start looking (default 30; use 65-70 at 4 MB)
--   CLICK     1 (default) = also click the centre and check the click was seen; 0 = skip
--   SHOT      screenshot name prefix (default "hello")
-- Prints "RESULT PASS" or "RESULT FAIL (n failures)" and exits MAME.
-- Self-contained (works on macplus and macsefd; emu/scripts/lib.lua only knows the Plus's mouse).
mem = manager.machine.devices[":maincpu"].spaces["program"]
screen = manager.machine.screens[":screen"]
local ports = manager.machine.ioport.ports
btn = (ports[":macadb:MOUSE0"] and ports[":macadb:MOUSE0"].fields["Mouse Button 0"])   -- SE (ADB)
   or ports[":MOUSE0"].fields["Mouse Button"]                                           -- Plus
local function mouse_to(h, v)   -- absolute cursor move via the low-memory mouse globals
  mem:write_u16(0x828, v); mem:write_u16(0x82A, h)
  mem:write_u16(0x82C, v); mem:write_u16(0x82E, h)
  mem:write_u8(0x8CE, mem:read_u8(0x8CF))
end
local function ram_top()        -- RAM size from MAME's -ramsize (MemTop at $108 isn't it under System 7)
  local n, u = manager.options.entries["ramsize"]:value():match("(%d+)(%a?)")
  return tonumber(n) * ((u == "M" or u == "m") and 1048576 or (u == "K" or u == "k") and 1024 or 1)
end

local MAGIC = 0x48504C53                         -- 'HPLS'
local EXPECT_W = tonumber(os.getenv("EXPECT_W") or "448")
local CHECK_AT = tonumber(os.getenv("CHECK_AT") or "30")
local CLICK = (os.getenv("CLICK") or "1") ~= "0"
local SHOT = os.getenv("SHOT") or "hello"
local fails = 0

local function check(ok, what)
  print(string.format("CHECK %-4s %s", ok and "ok" or "FAIL", what))
  if not ok then fails = fails + 1 end
end

local function scan()
  -- From the top of RAM down (app globals sit high on a Plus); step 2 because globals are only
  -- word-aligned (A5 was $0F6606 at 1 MB).
  for a = ram_top() - 8, 0x1000, -2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == 1 and mem:read_u32(a + 8) == (~MAGIC & 0xFFFFFFFF) then
      return a
    end
  end
end

local function rect(a) return mem:read_i16(a), mem:read_i16(a + 2), mem:read_i16(a + 4), mem:read_i16(a + 6) end

local blk, base, rb, bl, bt
local function px(h, v)  -- 1 = black; h, v in global (screenBits) coordinates
  local off = (v - bt) * rb + ((h - bl) >> 3)
  return (mem:read_u8(base + off) >> (7 - ((h - bl) & 7))) & 1
end

local function verify()
  local t, l, b, r = rect(blk + 16)
  bt, bl = t, l
  base, rb = mem:read_u32(blk + 24), mem:read_i16(blk + 28)
  local ft, fl, fb, fr = rect(blk + 32)
  print(string.format("block at %06x: state=%d bounds=(t%d,l%d,b%d,r%d) base=%06x rowBytes=%d frame=(t%d,l%d,b%d,r%d)",
    blk, mem:read_u32(blk + 12), t, l, b, r, base, rb, ft, fl, fb, fr))
  print(string.format("textWidth=%d ascent=%d drawTicks=%d A5=%06x FreeMem=%d ScrnBase=%06x RAM=%06x",
    mem:read_i16(blk + 40), mem:read_i16(blk + 42), mem:read_u32(blk + 44), mem:read_u32(blk + 48),
    mem:read_u32(blk + 52), mem:read_u32(0x824), ram_top()))
  check(t == 0 and l == 0 and b == 342 and r == EXPECT_W, string.format("screenBits.bounds = (0,0,342,%d)", EXPECT_W))
  check(fr - fl == 320 and fb - ft == 240, "frame is 320x240")
  check(fl - l == r - fr and ft - t == b - fb, string.format("frame centred: margins L%d R%d T%d B%d", fl - l, r - fr, ft - t, b - fb))
  local cx, cy = (l + r) // 2, (t + b) // 2
  -- pixels in the real frame buffer
  check(px(fl, cy + 20) == 1 and px(fl + 1, cy + 20) == 0 and px(fl - 1, cy + 20) == 0, "left frame edge black, neighbours white")
  check(px(fr - 1, cy + 20) == 1 and px(fr - 2, cy + 20) == 0 and px(fr, cy + 20) == 0, "right frame edge black, neighbours white")
  check(px(cx + 20, ft) == 1 and px(cx + 20, ft - 1) == 0 and px(cx + 20, fb - 1) == 1 and px(cx + 20, fb) == 0, "top/bottom frame edges")
  check(px(cx, ft + 20) == 1 and px(cx + 1, ft + 20) == 0 and px(fl + 20, cy) == 1 and px(fl + 20, cy + 1) == 0, "crosshair through the centre")
  local ink = 0
  for v = cy - 6, cy + 4 do for h = cx - 30, cx + 30 do ink = ink + px(h, v) end end
  check(ink > 40, string.format("text drawn at the centre (%d black px in 61x11)", ink))
  -- Narrow Screen margins: the physical screen is at RAM top - $5900, 64 bytes per row.
  local phys = ram_top() - 0x5900
  local lm = (base - phys) * 8
  local rm = 512 - lm - (r - l)
  print(string.format("physical screen %06x, margins left %d px, right %d px", phys, lm, rm))
  if EXPECT_W < 512 then
    local black = true
    for v = 30, 330, 50 do
      for byte = 0, lm // 8 - 1 do if mem:read_u8(phys + v * 64 + byte) ~= 0xFF then black = false end end
      for byte = 64 - rm // 8, 63 do if mem:read_u8(phys + v * 64 + byte) ~= 0xFF then black = false end end
    end
    check(black and lm > 0 and rm > 0, "margins outside screenBits are black (Narrow Screen active)")
  end
  return cx, cy
end

local state, next_scan, t_click, cx, cy = "scan", CHECK_AT, nil, nil, nil
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if state == "scan" and t >= next_scan then
    blk = scan()
    if blk and mem:read_u32(blk + 12) >= 1 then
      print(string.format("found 'HPLS' block at t=%.1fs emulated", t))
      cx, cy = verify()
      screen:snapshot(SHOT .. ".png")
      state = CLICK and "click" or "done"
      t_click = t + 0.5
    elseif t > CHECK_AT + 120 then
      print("block not found / not drawn (TIMEOUT)"); fails = fails + 1; state = "done"
    else
      next_scan = t + 5
    end
  elseif state == "click" and t >= t_click then
    mouse_to(cx, cy); btn:set_value(1); state = "release"; t_click = t + 0.2
  elseif state == "release" and t >= t_click then
    btn:clear_value(); state = "after"; t_click = t + 1.0
  elseif state == "after" and t >= t_click then
    local s = mem:read_u32(blk + 12)
    local cv, ch = mem:read_i16(blk + 56), mem:read_i16(blk + 58)
    check(s == 2 and math.abs(cv - cy) <= 1 and math.abs(ch - cx) <= 1,
      string.format("click seen: state=%d at (h%d,v%d), expected (h%d,v%d)", s, ch, cv, cx, cy))
    screen:snapshot(SHOT .. "_clicked.png")
    state = "done"
  end
  if state == "done" then
    print(fails == 0 and "RESULT PASS" or string.format("RESULT FAIL (%d failures)", fails))
    state = "exit"; manager.machine:exit()
  end
end)

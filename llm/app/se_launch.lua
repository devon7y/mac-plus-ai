-- Launch PixelSE from a floppy in the System 7 Finder (macsefd): double-click the floppy icon, then
-- the PixelSE icon. Env: DISK_ICON="h,v" APP_ICON="h,v" (global coordinates), T0 (s, desktop ready),
-- SHOTS_ONLY=1 (only screenshots, to find the icons). Then hands over to pixel_test.lua's checks:
-- run it with pixel_test.lua loaded after this file (see llm/app/se_floppy_test.sh).
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local p = manager.machine.ioport.ports
local btn = (p[":macadb:MOUSE0"] and p[":macadb:MOUSE0"].fields["Mouse Button 0"]) or p[":MOUSE0"].fields["Mouse Button"]
local function mouse_to(h, v)
  mem:write_u16(0x828, v); mem:write_u16(0x82A, h); mem:write_u16(0x82C, v); mem:write_u16(0x82E, h)
  mem:write_u8(0x8CE, mem:read_u8(0x8CF))
end
local function xy(s) local h, v = s:match("(%d+),(%d+)"); return tonumber(h), tonumber(v) end
local t0 = tonumber(os.getenv("T0") or "70")
local ev = {}
local function at(t, fn) ev[#ev + 1] = {t, fn} end
local function dclick(t, h, v)
  at(t, function() mouse_to(h, v) end)
  at(t + 0.3, function() btn:set_value(1) end)
  at(t + 0.4, function() btn:clear_value() end)
  at(t + 0.55, function() btn:set_value(1) end)
  at(t + 0.65, function() btn:clear_value() end)
end
local function shot(t, name) at(t, function() manager.machine.video:snapshot(); print("SHOT " .. name) end) end
if os.getenv("INSERT") then          -- insert the floppy after the Finder is up (a non-bootable disk
  at(t0 - 12, function()             -- in the drive at startup is ejected by the ROM)
    for tag, img in pairs(manager.machine.images) do
      if tag:match("^:fdc:0:") then img:load(os.getenv("INSERT")); print("INSERT " .. tag) end
    end
  end)
end
if os.getenv("SHOTS_ONLY") then
  shot(t0, "desktop")
  if os.getenv("DISK_ICON") then dclick(t0 + 2, xy(os.getenv("DISK_ICON"))); shot(t0 + 8, "disk_window") end
else
  dclick(t0, xy(os.getenv("DISK_ICON")))
  shot(t0 + 6, "disk_window")
  dclick(t0 + 8, xy(os.getenv("APP_ICON")))
  shot(t0 + 12, "launching")
end
table.sort(ev, function(a, b) return a[1] < b[1] end)
local i = 1
launch_sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  while i <= #ev and t >= ev[i][1] do ev[i][2](); i = i + 1 end
end)

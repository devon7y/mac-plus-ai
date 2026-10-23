-- Finder screenshots for icon checks (macplus or macsefd, System 6 or 7). Waits for the Finder,
-- optionally inserts a floppy, then double-clicks a list of points (disk icons, folders), clicks a
-- zoom box, and takes a screenshot after each step (shots/view_N.png); then quits MAME.
--   INSERT=<image>     floppy to insert once the Finder is up (drive DRIVE, default 0)
--   STEPS="d:472,52;d:60,90;z:408,51;w:5"  d = double-click, c = click, z = click (zoom box),
--                      w = wait N seconds; screen coordinates are global (512 x 342)
--   GAP=6              seconds between steps
-- Run: MACHINE=macplus RAM=1M HARDDISK=x.hd STEPS=... emu/run_test.sh none toolchain/icons/finder_view.lua 200
dofile(os.getenv("EMU") .. "/../toolchain/se7/se_lib.lua")
local gap = tonumber(os.getenv("GAP") or "6")
local steps = {}
for kind, a, b in (os.getenv("STEPS") or ""):gmatch("(%a):([%d.]+),?(%d*)") do
  table.insert(steps, {kind, tonumber(a), tonumber(b)})
end
local plan, n = {}, 0
local function add(t, f) table.insert(plan, {t, f}) end
local function shot() n = n + 1; screen:snapshot(string.format("view_%d.png", n)); print("VIEW shot " .. n) end
local t0 = 3
add(t0 - 1, shot)
if os.getenv("INSERT") then
  add(t0, function()
    local d = floppy(tonumber(os.getenv("DRIVE") or "0"))
    d:load(os.getenv("INSERT")); print("VIEW inserted " .. os.getenv("INSERT"))
  end)
  t0 = t0 + 12
  add(t0 - 1, shot)
end
for _, s in ipairs(steps) do
  local kind, x, y = s[1], s[2], s[3]
  if kind == "w" then
    t0 = t0 + x
  else
    add(t0, function() mouse_to(x, y) end)
    add(t0 + 0.05, function() btn:set_value(1) end)
    add(t0 + (kind == "z" and 0.4 or 0.12), function() btn:clear_value() end)
    if kind == "d" then
      add(t0 + 0.2, function() btn:set_value(1) end)
      add(t0 + 0.27, function() btn:clear_value() end)
    end
    t0 = t0 + gap
    add(t0 - 0.5, shot)
  end
end
add(t0 + 0.5, function() print("VIEW done"); manager.machine:exit() end)
table.sort(plan, function(p, q) return p[1] < q[1] end)

local finder_at, i = nil, 1
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not finder_at then
    if app_name() == "Finder" then finder_at = t; print(string.format("VIEW Finder at t=%.1f", t)) end
    if t > 400 then print("VIEW timeout waiting for the Finder"); manager.machine:exit() end
    return
  end
  while i <= #plan and t - finder_at >= plan[i][1] do plan[i][2](); i = i + 1 end
end)

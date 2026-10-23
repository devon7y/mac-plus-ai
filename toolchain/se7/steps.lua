-- Run a list of timed actions from a text file ($STEPS), one per line:
--   <t> click <h> <v>       move the mouse and click
--   <t> dclick <h> <v>      double-click
--   <t> menu <h> <v> <h2> <v2>   press at (h, v) (a menu title), drag to (h2, v2), release
--   <t> drag <h> <v> <h2> <v2>   press at (h, v), move in 20 steps over 1 s to (h2, v2), release
--   <t> snap <name>         screenshot (shots/<name>.png) + print app name
--   <t> insert <0|1> <img>  load a floppy image into drive 0/1
--   <t> exit
-- Example: STEPS=my.steps MACHINE=macsefd emu/run_test.sh none toolchain/se7/steps.lua 200
dofile(os.getenv("EMU") .. "/../toolchain/se7/se_lib.lua")
local steps = {}
for line in io.lines(os.getenv("STEPS")) do
  local t, cmd, rest = line:match("^%s*([%d%.]+)%s+(%a+)%s*(.*)$")
  if t then
    t = tonumber(t)
    if cmd == "click" then
      local h, v = rest:match("(%d+)%s+(%d+)")
      h, v = tonumber(h), tonumber(v)
      steps[#steps + 1] = {t, function() mouse_to(h, v) end}
      steps[#steps + 1] = {t + 0.1, function() btn:set_value(1) end}
      steps[#steps + 1] = {t + 0.25, function() btn:clear_value() end}
    elseif cmd == "dclick" then
      local h, v = rest:match("(%d+)%s+(%d+)")
      h, v = tonumber(h), tonumber(v)
      steps[#steps + 1] = {t, function() mouse_to(h, v) end}
      steps[#steps + 1] = {t + 0.05, function() btn:set_value(1) end}
      steps[#steps + 1] = {t + 0.12, function() btn:clear_value() end}
      steps[#steps + 1] = {t + 0.2, function() btn:set_value(1) end}
      steps[#steps + 1] = {t + 0.27, function() btn:clear_value() end}
    elseif cmd == "menu" then
      local h, v, h2, v2 = rest:match("(%d+)%s+(%d+)%s+(%d+)%s+(%d+)")
      h, v, h2, v2 = tonumber(h), tonumber(v), tonumber(h2), tonumber(v2)
      steps[#steps + 1] = {t, function() mouse_to(h, v) end}
      steps[#steps + 1] = {t + 0.05, function() btn:set_value(1) end}
      steps[#steps + 1] = {t + 0.5, function() mouse_to(h2, v2) end}
      steps[#steps + 1] = {t + 0.9, function() btn:clear_value() end}
    elseif cmd == "drag" then
      local h, v, h2, v2 = rest:match("(%d+)%s+(%d+)%s+(%d+)%s+(%d+)")
      h, v, h2, v2 = tonumber(h), tonumber(v), tonumber(h2), tonumber(v2)
      steps[#steps + 1] = {t, function() mouse_to(h, v) end}
      steps[#steps + 1] = {t + 0.05, function() btn:set_value(1) end}
      for k = 1, 20 do
        steps[#steps + 1] = {t + 0.05 + k * 0.05, function()
          mouse_to(h + (h2 - h) * k // 20, v + (v2 - v) * k // 20) end}
      end
      steps[#steps + 1] = {t + 1.2, function() btn:clear_value() end}
    elseif cmd == "snap" then
      steps[#steps + 1] = {t, function() print(string.format("t=%.1f app=%q", t, app_name())); screen:snapshot(rest .. ".png") end}
    elseif cmd == "insert" then
      local d, img = rest:match("(%d)%s+(.+)")
      steps[#steps + 1] = {t, function() local f = floppy(tonumber(d)); print("insert", img, f:load(img)) end}
    elseif cmd == "exit" then
      steps[#steps + 1] = {t, function() manager.machine:exit() end}
    end
  end
end
table.sort(steps, function(a, b) return a[1] < b[1] end)
run_steps(steps)

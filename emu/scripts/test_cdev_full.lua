dofile(os.getenv("EMU") .. "/scripts/lib.lua")
local steps = {
  { 28.0, function() mouse_to(12, 8) end }, { 28.3, function() btn:set_value(1) end },
  { 28.8, function() mouse_to(60, 107) end }, { 29.3, function() btn:clear_value() end },
}
local function click(t, h, v, shot)
  table.insert(steps, { t, function() mouse_to(h, v) end })
  table.insert(steps, { t + 0.2, function() btn:set_value(1) end })
  table.insert(steps, { t + 0.4, function() btn:clear_value() end })
  if shot then table.insert(steps, { t + 2.5, function() screen:snapshot(shot) end }) end
end
click(45, 98, 258)                              -- Narrow Screen icon
click(56, 325, 130, "full_1_left_plus.png")     -- Left bar: right arrow  -> 48
click(60, 219, 158, "full_2_right_minus.png")   -- Right bar: left arrow  -> 31
click(64, 298, 158, "full_3_right_page.png")    -- Right bar: page right  -> 39
click(68, 328, 281)                             -- Restart
table.insert(steps, { 110, function() globals("after restart"); screen:snapshot("full_4_after_restart.png") end })
table.insert(steps, { 110.1, function() manager.machine:exit() end })
table.sort(steps, function(a, b) return a[1] < b[1] end)
run_steps(steps)

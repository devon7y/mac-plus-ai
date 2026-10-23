dofile(os.getenv("EMU") .. "/scripts/lib.lua")
local function click(h, v, t)  -- returns steps for a click at time t
  return { { t, function() mouse_to(h, v) end }, { t + 0.2, function() btn:set_value(1) end }, { t + 0.4, function() btn:clear_value() end } }
end
local steps = {
  { 28.0, function() mouse_to(12, 8) end },
  { 28.3, function() btn:set_value(1) end },
  { 28.8, function() mouse_to(60, 107) end },
  { 29.3, function() btn:clear_value() end },
}
for _, s in ipairs(click(98, 258, 45)) do table.insert(steps, s) end      -- "Narrow Screen" icon
table.insert(steps, { 55, function() globals("cdev open"); screen:snapshot("real_cdev.png") end })
table.insert(steps, { 55.1, function() manager.machine:exit() end })
run_steps(steps)

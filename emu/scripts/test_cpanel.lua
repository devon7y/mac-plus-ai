dofile(os.getenv("EMU") .. "/scripts/lib.lua")
run_steps({
  { 28.0, function() mouse_to(12, 8) end },
  { 28.3, function() btn:set_value(1) end },          -- press Apple menu
  { 28.8, function() mouse_to(60, 107) end },         -- drag to "Control Panel"
  { 29.3, function() btn:clear_value() end },         -- release
  { 45.0, function() screen:snapshot("real_cpanel.png") end },
  { 45.1, function() manager.machine:exit() end },
})

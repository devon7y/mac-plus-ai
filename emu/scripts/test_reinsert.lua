dofile(os.getenv("EMU") .. "/scripts/autoreinsert.lua")
sub_reinsert = sub
dofile(os.getenv("EMU") .. "/scripts/lib.lua")
local steps = {}
local function at(t, f) steps[#steps + 1] = { t, f } end
local function click(t, h, v) at(t, function() mouse_to(h, v) end); at(t + 0.2, function() btn:set_value(1) end); at(t + 0.4, function() btn:clear_value() end) end
at(28.0, function() mouse_to(12, 8) end); at(28.3, function() btn:set_value(1) end)
at(28.8, function() mouse_to(60, 107) end); at(29.3, function() btn:clear_value() end)
click(45, 98, 258)        -- Narrow Screen
click(56, 328, 281)       -- Restart
at(105, function() globals("after restart, no manual reinsert"); screen:snapshot("reinsert_after.png") end)
at(105.1, function() manager.machine:exit() end)
table.sort(steps, function(a, b) return a[1] < b[1] end)
run_steps(steps)

dofile(os.getenv("EMU") .. "/scripts/lib.lua")
local disk = os.getenv("DISK")
local floppy
for tag, img in pairs(manager.machine.images) do if tag:find("fdc:0") then floppy = img end end
local steps = {}
local function at(t, f) steps[#steps + 1] = { t, f } end
local function click(t, h, v) at(t, function() mouse_to(h, v) end); at(t + 0.2, function() btn:set_value(1) end); at(t + 0.4, function() btn:clear_value() end) end
local function shot(t, name) at(t, function() screen:snapshot(name) end) end
at(28.0, function() mouse_to(12, 8) end); at(28.3, function() btn:set_value(1) end)
at(28.8, function() mouse_to(60, 107) end); at(29.3, function() btn:clear_value() end)
click(45, 98, 258)                      -- select Narrow Screen
shot(55, "e2e_0_open.png")
click(56, 325, 130); shot(59, "e2e_1_left_arrow.png")      -- Left +1 step  -> 48
click(60, 219, 158); shot(63, "e2e_2_right_arrow.png")     -- Right -1      -> 31
click(64, 298, 158); shot(67, "e2e_3_right_page.png")      -- Right +8      -> 39
-- drag Left thumb (value 3, center x=263) right by 18 px -> value 5 -> 80
at(68.0, function() mouse_to(263, 130) end); at(68.2, function() btn:set_value(1) end)
at(68.6, function() mouse_to(272, 130) end); at(69.0, function() mouse_to(281, 130) end)
at(69.4, function() btn:clear_value() end); shot(72, "e2e_4_left_drag.png")
click(73, 328, 281)                     -- Restart (ejects the floppy)
at(80, function() print("floppy exists after restart: " .. tostring(floppy.exists)); screen:snapshot("e2e_5_needs_disk.png"); floppy:load(disk) end)
at(115, function() globals("after reboot"); screen:snapshot("e2e_6_after_reboot.png") end)
at(115.1, function() manager.machine:exit() end)
table.sort(steps, function(a, b) return a[1] < b[1] end)
run_steps(steps)

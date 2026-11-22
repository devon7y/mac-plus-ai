-- The lab-SE step in MAME (SE / System 7.0.1 testbed, Disk Copy 4.2 as the startup shell, "Plus Modem.image"
-- at the top of the hard disk): dismiss the title screen, Load Image File... -> Plus Modem.image, Make A Copy,
-- insert a blank 800K floppy (BLANK=<image>) when asked, wait for the copy. Screenshots of each stage; the
-- written floppy is the run's copy of BLANK (see verify_copy in shuttle/tests/README or the HANDOFF log).
dofile(os.getenv("SHUTTLE_TESTS") .. "/lib.lua")
no_block()
local function fd(i)
  for tag, img in pairs(manager.machine.images) do
    if tag:match("^:fdc:" .. i .. ":") then return img end
  end
end
local function click(h, v)
  return {
    {function() return true end, function() mouse_to(h, v) end, 0.4},
    {function() return true end, function() btn:set_value(1) end, 0.3},
    {function() return true end, function() btn:clear_value() end, 0.5},
  }
end
local steps = {{function() return now() > 50 end, function() shot("1_splash") end, 1}}
local function add(l) for _, s in ipairs(l) do steps[#steps + 1] = s end end
add(click(256, 200))                                               -- title screen
add({{function() return true end, function() shot("2_window") end, 1}})
add(click(176, 253))                                               -- Load Image File...
add({{function() return true end, function() shot("3_open") end, 1}})
add(click(366, 144))                                               -- Desktop
add({{function() return true end, function() type_text("Mac") end, 1.5},
     {function() return true end, function() key("Return") end, 2},  -- open Macintosh HD
     {function() return true end, function() type_text("Plus") end, 1.5},
     {function() return true end, function() shot("3b_choose") end, 0.5},
     {function() return true end, function() key("Return") end, 25},  -- open the image
     {function() return true end, function() shot("4_loaded") end, 1}})
add(click(338, 213))                                               -- Make A Copy
add({{function() return true end, function() shot("5_insert_prompt") end, 1},
     {function() return true end, function()
        local f = fd(0)
        print("insert blank:", f and f:load(os.getenv("BLANK")))
      end, 90},
     {function() return true end, function() shot("6_after_copy") end, 30},
     {function() return true end, function() shot("7_later"); print("RESULT DONE"); manager.machine:exit() end}})
run_steps(steps)

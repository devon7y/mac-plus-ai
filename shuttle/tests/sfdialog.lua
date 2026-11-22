-- Redraw check: a Standard File dialog closes and a transfer starts at once; the window must be redrawn.
-- (Send File... with "Upload Test" from the test floppy, then a few seconds of sending.)
dofile(os.getenv("SHUTTLE_TESTS") .. "/lib.lua")
local r0 = 0
run_steps({
  {function() return base and i32(16) == 2 and i32(108) >= 1 end, function() report("listed") end, 2},
  {function() return true end, function() r0 = i32(332); cmd_key("u") end, 3},
  {function() return true end, function() type_text("Up") end, 1.5},
  {function() return true end, function() key("Return") end, 4},
  {function() return true end, function()
    shot("after"); report("after")
    print(string.format("redraws before %d after %d", r0, i32(332)))
    print(i32(332) > r0 and "RESULT PASS" or "RESULT FAIL"); manager.machine:exit() end},
})

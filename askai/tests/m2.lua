-- Ask AI M2 check in MAME: Settings dialog (opened from the menu, OK), the Model menu (switch to the
-- second model), a provider error shown as a note, a reply, Disconnect (Command-K: BYE, the relay
-- hangs up, NO CARRIER, port closed), then Connection > Modem Terminal and ATI (the modem answers).
-- Menu positions are for the 448 px Narrow Screen layout.
dofile(os.getenv("ASKAI_TESTS") .. "/lib.lua")
local CONN_H, MODEL_H = 150, 215
local errs0, replies0 = 0, 0
local steps = {
  {function() return base and i32(12) == 10 end, function() report("ready") end, 1},
}
local function add(list) for _, s in ipairs(list) do steps[#steps + 1] = s end end
add(menu_steps(CONN_H, 92))                                              -- Connection > Settings...
add({{function() return true end, function() end, 4},                  -- let the dialog draw
     {function() return true end, function() shot("m2_1_settings") end, 1},
     {function() return true end, function() key("Return") end, 2}})     -- OK
add(menu_steps(MODEL_H, 44))                                             -- Model > second model
add({
  {function() return str(132, 32) == "mock-fast" end, function() report("model switched") end, 1},
  {function() return true end, function() errs0 = i32(28); type_text("please fail !error") end, 4},
  {function() return true end, function() key("Return") end, 1},
  {function() return i32(28) > errs0 and i32(12) == 10 end, function() report("error shown") end, 1},
  {function() return true end, function() replies0 = i32(24); type_text("and now a real one") end, 4},
  {function() return true end, function() key("Return") end, 1},
  {function() return i32(24) > replies0 and i32(12) == 10 end, function() report("reply"); shot("m2_2_error_and_reply") end, 1},
  {function() return true end, function() cmd_key("k") end, 1},
  {function() return i32(12) == 0 end, function() report("disconnected") end, 1},
  {function() return true end, function() shot("m2_3_disconnected") end, 1},
})
local rx0, disc_ok = 0, false
add(menu_steps(CONN_H, 76))                                              -- Connection > Modem Terminal
add({
  {function() return i32(12) == 16 end, function()
    disc_ok = str(132, 32) == "mock-fast" and i32(32) == 8 and i32(52) == 0 and i32(24) > replies0
    rx0 = i32(16); type_text("ATI") end, 3},
  {function() return true end, function() key("Return") end, 3},
  {function() return true end, function()
    report("terminal"); shot("m2_4_terminal")
    local ok = disc_ok and i32(16) > rx0 + 20
    print(ok and "RESULT PASS" or "RESULT FAIL")
    manager.machine:exit()
  end},
})
run_steps(steps)

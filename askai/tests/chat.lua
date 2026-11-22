-- Ask AI M1 check in MAME: the app on the test floppy (prefs dial the relay through the modem emulator)
-- connects by itself; two messages, two streamed replies. Run by askai/tests/run_chain.sh.
-- Env: BAUD, MSG1, MSG2, LIMIT (see lib.lua).
dofile(os.getenv("ASKAI_TESTS") .. "/lib.lua")
local msg1 = os.getenv("MSG1") or "Hello from a Mac Plus!"
local msg2 = os.getenv("MSG2") or "what is it good at?"
local replies0 = 0
run_steps({
  {function() return base and i32(12) == 10 end, function() report("ready"); shot("1_connected") end, 1},
  {function() return true end, function() type_text(msg1) end, 4},
  {function() return true end, function() replies0 = i32(24); key("Return") end, 0.5},
  {function() return i32(12) == 11 end, function() report("streaming") end, 1.5},
  {function() return true end, function() shot("2_streaming") end},
  {function() return i32(24) > replies0 and i32(12) == 10 end, function() report("reply1") end, 1},
  {function() return true end, function() shot("3_reply1") end},
  {function() return true end, function() type_text(msg2) end, 4},
  {function() return true end, function() replies0 = i32(24); key("Return") end, 1},
  {function() return i32(24) > replies0 and i32(12) == 10 end, function() report("reply2") end, 1},
  {function() return true end, function()
    shot("4_reply2")
    local ok = i32(24) >= 2 and i32(28) == 0 and i32(40) >= 1 and i32(52) == 0
    print(ok and "RESULT PASS" or "RESULT FAIL")
    manager.machine:exit()
  end},
})

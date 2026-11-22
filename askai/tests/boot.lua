-- Ask AI boot check (no relay): the floppy starts the app, which shows its welcome note.
--   MACHINE=macsefd RAM=4M emu/run_test.sh askai/build/release/askai_se.img askai/tests/boot.lua 150
dofile(os.getenv("ASKAI_TESTS") .. "/lib.lua")
run_steps({
  {function() return base and i32(36) >= 1 end, function() end, 3},
  {function() return true end, function()
    report("booted"); shot("boot")
    local ok = i32(12) == 0 and str(68, 64):find("Welcome") ~= nil and i32(28) == 0
    print(ok and "RESULT PASS" or "RESULT FAIL")
    manager.machine:exit()
  end},
})

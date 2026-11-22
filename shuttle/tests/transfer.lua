-- Shuttle check in MAME (run by shuttle/tests/run_chain.sh): the test floppy's prefs dial "mac" at the
-- MAME test speed and connect at startup. Lists the top folder, opens "Test Files", downloads "Big Test"
-- (about 300 KB, both forks) to the startup disk, sends "Upload Test" to the MacBook, then starts a second
-- download and cancels it with the Cancel button (Command-period on the SE, whose ADB mouse the script can't click). verify.py then compares the files on both sides.
-- Debug block offsets: app/main.c struct Dbg (320: the window's top, left, height, width).
dofile(os.getenv("SHUTTLE_TESTS") .. "/lib.lua")
local function i16(off) local v = mem:read_u16(base + off); if v >= 0x8000 then v = v - 0x10000 end; return v end
local function cancel_h() return i16(322) + i16(326) - 8 - 36 end    -- the Cancel button's centre
local function cancel_v() return i16(320) + i16(324) - 20 end
local function linkst() return i32(12) end
local function pst() return i32(16) end
local function job() return i32(20) end
local t_start = 0
local steps = {
  {function() return base and pst() == 2 and i32(108) >= 1 end, function() report("listed top"); shot("1_top") end, 1},
  {function() return true end, function() type_text("T") end, 1},
  {function() return i32(36) >= 0 end, function() key("Return") end, 1},
  {function() return i32(108) >= 2 and str(176, 64) == "Test Files" and pst() == 2 end, function()
    report("opened Test Files"); shot("2_folder") end, 1},
  {function() return true end, function() key("Return") end, 4},                       -- Download -> Save dialog
  {function() return true end, function() shot("3_save_dialog") end, 0.5},
  {function() return true end, function() key("Return") end, 1},                       -- Save
  {function() return job() == 3 and pst() == 4 end, function() t_start = now(); report("downloading") end, 30},
  {function() return not os.getenv("STOP_AT_BYTES") or i32(40) > tonumber(os.getenv("STOP_AT_BYTES")) end, function()
    if os.getenv("STOP_AT_BYTES") then report("stopping"); print("RESULT STOPPED"); manager.machine:exit() end end},
  {function() return true end, function() shot("4_downloading") end, 1},
  {function() return i32(68) >= 1 and pst() == 2 end, function()
    report(string.format("downloaded in %.1f s emulated", now() - t_start)); shot("5_downloaded")
    if os.getenv("STOP_AFTER") == "download" then print("RESULT STOPPED"); manager.machine:exit() end end, 2},
  {function() return true end, function() cmd_key("u") end, 4},                       -- Send File... -> Open dialog
  {function() return true end, function() type_text("Up") end, 2},
  {function() return true end, function() shot("6_open_dialog") end, 0.5},
  {function() return true end, function() key("Return") end, 1},
  {function() return i32(72) >= 1 and pst() == 2 end, function() report("uploaded"); shot("7_uploaded") end, 3},
}
local function add(list) for _, s in ipairs(list) do steps[#steps + 1] = s end end
add({
  {function() return pst() == 2 end, function() key("Return") end, 4},               -- download again ...
  {function() return true end, function() type_text("Big Two") end, 2},              -- ... under another name
  {function() return true end, function() key("Return") end, 1},
  {function() return pst() == 4 and i32(40) > 20000 end, function() report("second download running"); shot("8_running") end, 0.5},
})
add({                                                                             -- Cancel: the button on the
  {function() return true end, function()                                        -- Plus (mouse), Command-. on
    if btn then mouse_to(cancel_h(), cancel_v()); btn:set_value(1) else cmd_key(".") end   -- the SE (ADB mouse)
  end, 0.3},
  {function() return true end, function() if btn then btn:clear_value() end end, 0.5},
})
add({
  {function() return i32(76) >= 1 and pst() == 2 end, function() report("cancelled"); shot("9_cancelled") end, 2},
  {function() return true end, function()
    report("end")
    local ok = i32(68) == 1 and i32(72) == 1 and i32(76) == 1 and i32(80) == 0 and i32(52) == 0
    print(ok and "RESULT PASS" or "RESULT FAIL")
    manager.machine:exit()
  end},
})
run_steps(steps)

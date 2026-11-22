-- Boot check of the Plus Modem floppy (run from its Disk Copy 4.2 image): the Finder comes up, the disk's
-- window is opened, then Shuttle and Ask AI are each opened (select by typing, Command-O) and found running
-- (their debug blocks 'SHT!' and 'ASK!'), then quit. Positions are logical coordinates on the 448 px
-- Narrow Screen (screenshot x minus 32): the disk icon, then the icons in the disk's window (System 6's
-- Finder has no type-to-select, so they are double-clicked).
dofile(os.getenv("SHUTTLE_TESTS") .. "/lib.lua")
local DH, DV = tonumber(os.getenv("DISK_H") or "410"), tonumber(os.getenv("DISK_V") or "35")
local function find(magic, notmagic)
  for a = 0x8000, 0xFFFF0, 2 do
    if mem:read_u32(a) == magic and mem:read_u32(a + 8) == notmagic then return a end
  end
end
local seen = {}
local function dbl(h, v)
  return {
    {function() return true end, function() mouse_to(h, v) end, 0.3},
    {function() return true end, function() btn:set_value(1) end, 0.1},
    {function() return true end, function() btn:clear_value() end, 0.1},
    {function() return true end, function() btn:set_value(1) end, 0.1},
    {function() return true end, function() btn:clear_value() end, 3},
  }
end
local steps = {
  {function() return now() > 30 end, function() shot("1_desktop") end, 1},
}
local function add(l) for _, s in ipairs(l) do steps[#steps + 1] = s end end
add(dbl(DH, DV))
add({
  {function() return true end, function() shot("2_disk_window") end, 1},
})
add(dbl(301, 100))                                                  -- Shuttle
add({
  {function() return true end, function() end, 10},
  {function() return true end, function() seen.shuttle = find(0x53485421, 0xACB7ABDE) ~= nil; shot("3_shuttle") end, 1},
  {function() return true end, function() cmd_key("q") end, 8},
})
add(dbl(174, 100))                                                  -- Ask AI
add({
  {function() return true end, function() end, 10},
  {function() return true end, function() seen.askai = find(0x41534B21, 0xBEACB4DE) ~= nil; shot("4_askai") end, 1},
  {function() return true end, function()
    print(string.format("shuttle running: %s, ask ai running: %s", tostring(seen.shuttle), tostring(seen.askai)))
    print((seen.shuttle and seen.askai) and "RESULT PASS" or "RESULT FAIL"); manager.machine:exit() end},
})
no_block()                                 -- run_steps would otherwise wait for Shuttle's debug block
run_steps(steps)

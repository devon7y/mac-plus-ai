-- Debug helper: screenshots at SHOTS (comma list of seconds), full-RAM scan for TAG at each.
dofile(os.getenv("EMU") .. "/scripts/lib.lua")
local shots = {}
for s in (os.getenv("SHOTS") or "70,100,150"):gmatch("[^,]+") do shots[#shots + 1] = tonumber(s) end
local tag = os.getenv("TAG") or "BWBN"
local magic = 0
for i = 1, 4 do magic = magic * 256 + tag:byte(i) end
local i = 1
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if i <= #shots and t >= shots[i] then
    screen:snapshot(string.format("peek_%d.png", shots[i]))
    local found = nil
    local top = mem:read_u32(0x108)
    for a = 0x1000, top - 8, 2 do
      if mem:read_u32(a) == magic and mem:read_u32(a + 4) == (0xFFFFFFFF - magic) then found = a; break end
    end
    print(string.format("t=%.0f A5=%06x CurApName=%s found=%s state=%s", t, mem:read_u32(0x904),
      (function() local n = mem:read_u8(0x910); local s = ""; for k = 1, n do s = s .. string.char(mem:read_u8(0x910 + k)) end; return s end)(),
      found and string.format("%06x", found) or "no", found and mem:read_u32(found + 8) or "-"))
    i = i + 1
    if i > #shots then manager.machine:exit() end
  end
end)

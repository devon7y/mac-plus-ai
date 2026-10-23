dofile(os.getenv("EMU") .. "/scripts/lib.lua")
-- node 7 descriptor: flink=8, blink=6, kind=0xFF, height=1, nrec=4
local sig = { 0,0,0,8, 0,0,0,6, 0xFF, 1, 0, 4 }
local last = ""
local function scan(t)
  local hits = {}
  for a = 0, 0xFFFF0, 2 do
    if mem:read_u8(a + 3) == 8 and mem:read_u8(a + 7) == 6 and mem:read_u8(a + 8) == 0xFF and mem:read_u8(a + 9) == 1 and mem:read_u16(a + 10) == 4
       and mem:read_u32(a) == 8 and mem:read_u32(a + 4) == 6 then
      local name = ""; for i = 0, 11 do local c = mem:read_u8(a + 21 + i); if c >= 32 and c < 127 then name = name .. string.char(c) end end
      hits[#hits + 1] = string.format("%06x(%s)", a, name)
    end
  end
  local s = table.concat(hits, " ")
  if s ~= last then print(string.format("t=%5.2f ScrnBase=%06x BufPtr=%06x node7 copies: %s", t, mem:read_u32(0x824), mem:read_u32(0x10C), s)); last = s end
end
local steps = {}
for t = 12, 30, 0.25 do steps[#steps + 1] = { t, function() scan(t) end } end
steps[#steps + 1] = { 30.5, function() manager.machine:exit() end }
run_steps(steps)

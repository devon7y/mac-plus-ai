-- Boot a disk, then report the running app and memory layout, with screenshots.
dofile(os.getenv("EMU") .. "/scripts/lib.lua")
local tag = os.getenv("TAG") or "boot"
local function report()
  local n = mem:read_u8(0x910); local name = ""
  for i = 1, n do name = name .. string.char(mem:read_u8(0x910 + i)) end
  local sys, appl, buf = mem:read_u32(0x2A6), mem:read_u32(0x2AA), mem:read_u32(0x10C)
  print(string.format("[%s] running app: %q | system heap %dK | application space %dK", tag, name, (appl - sys) // 1024, (buf - appl) // 1024))
end
run_steps({
  { 20, function() screen:snapshot(tag .. "_20s.png") end },
  { 40, function() report(); screen:snapshot(tag .. "_40s.png") end },
  { 40.1, function() manager.machine:exit() end },
})

dofile(os.getenv("EMU") .. "/scripts/lib.lua")
run_steps({ { 40, function()
  print(string.format("[%s] MemTop=%dK ScrnBase=%06x BufPtr=%06x ApplZone=%06x -> app space %dK",
    os.getenv("TAG"), mem:read_u32(0x108) // 1024, mem:read_u32(0x824), mem:read_u32(0x10C), mem:read_u32(0x2AA),
    (mem:read_u32(0x10C) - mem:read_u32(0x2AA)) // 1024))
  manager.machine:exit() end } })

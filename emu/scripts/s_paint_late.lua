dofile(os.getenv("EMU") .. "/scripts/lib.lua")
local paint_at = tonumber(os.getenv("PAINT_AT") or "30")
run_steps({
  { paint_at, function()
      local base = mem:read_u32(0x824) & 0xFFFF80
      for y = 0, 341 do for b = 0, 3 do mem:write_u8(base + y*64 + b, 0xFF) end; for b = 60, 63 do mem:write_u8(base + y*64 + b, 0xFF) end end
      print(string.format("painted margins at t=%.1f (base %06x)", paint_at, base))
    end },
  { 40, function() manager.machine:exit() end },
})

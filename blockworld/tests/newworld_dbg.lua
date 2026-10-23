-- Blockworld: make a world with debug command 11 (SEED, default 57418) and report the player and the view.
local cpu = manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local screen = manager.machine.screens[":screen"]
local MAGIC, NOTMAGIC = 0x4257474D, 0xBDA8B8B2
local blk, next_scan, phase, t0 = nil, 30, 0, 0
local seed = tonumber(os.getenv("SEED") or "57418")
local function scan()
  for a = 0x1000, mem:read_u32(0x108) - 8, 2 do
    if mem:read_u32(a) == MAGIC and mem:read_u32(a + 4) == NOTMAGIC then return a end
  end
end
local function rep(what)
  print(string.format("%s: seed %d, feet (%.2f, %.2f, %.2f) yaw %d pitch %d aim %d, ground %d, frames %d refines %d, view %d x %d, sky? state %x",
    what, mem:read_i32(blk + 292), mem:read_i32(blk + 16) / 256, mem:read_i32(blk + 20) / 256, mem:read_i32(blk + 24) / 256,
    mem:read_i16(blk + 28), mem:read_i16(blk + 30), mem:read_i16(blk + 270), mem:read_i16(blk + 78),
    mem:read_u32(blk + 32), mem:read_u32(blk + 40), mem:read_i16(blk + 266), mem:read_i16(blk + 268), mem:read_u32(blk + 8)))
end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not blk then
    if t >= next_scan then blk = scan(); next_scan = t + 0.5 end
    return
  end
  if phase == 0 and mem:read_u32(blk + 8) == 2 and mem:read_u32(blk + 40) >= 1 then
    rep("start"); screen:snapshot("a_start.png")
    mem:write_u16(blk + 214, 1)
    mem:write_u16(blk + 164, seed); mem:write_u16(blk + 162, 11); phase = 1; t0 = t
  elseif phase == 1 and mem:read_u16(blk + 162) == 0 and mem:read_u32(blk + 8) == 2 then
    phase = 2; t0 = t
  elseif phase == 2 and t - t0 > 3 then
    rep("new world"); screen:snapshot("b_new.png")
    mem:write_u16(blk + 164, 64); mem:write_u16(blk + 166, 64); mem:write_u16(blk + 168, 256); mem:write_u16(blk + 162, 2)
    phase = 3; t0 = t
  elseif phase == 3 and t - t0 > 3 then
    rep("stand at 64, 64 facing +z"); screen:snapshot("c_stand.png")
    manager.machine:exit()
  end
end)

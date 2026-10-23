-- Frame rate with the sound DMA enabled vs disabled (VIA port B bit 7), autopilot driving.
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local base, t0, mark
local function scan()
  for a = 0x1000, 0xFFFF0, 2 do
    if mem:read_u32(a) == 0x454E4455 and mem:read_u32(a + 8) == 0xBAB1BBAA then return a end
  end
end
local steps = {}
local function fps(tag) local fr, tk = mem:read_u32(base + 12) - mark[1], mem:read_u32(base + 16) - mark[2]
  print(string.format("FPS %s: %.1f (VIA ORB=%02x)", tag, fr / (tk / 60.15), mem:read_u8(0xEFE1FE))) end
local function m() mark = {mem:read_u32(base + 12), mem:read_u32(base + 16)} end
sub = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if not base then if t > 20 then base = scan(); t0 = t; mem:write_u32(base + 48, 77); mem:write_u16(base + 54, 1); mem:write_u16(base + 52, 1) end return end
  local e = t - t0
  if e > 8 and not steps[1] then steps[1] = 1; m() end
  if e > 28 and not steps[2] then steps[2] = 1; fps("sound on"); mem:write_u8(0xEFE1FE, mem:read_u8(0xEFE1FE) | 0x80); m() end
  if e > 48 and not steps[3] then steps[3] = 1; fps("sound off"); mem:write_u8(0xEFE1FE, mem:read_u8(0xEFE1FE) & 0x7F); m() end
  if e > 68 then fps("sound on again"); manager.machine:exit() end
end)

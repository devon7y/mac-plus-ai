-- Dump memory: DUMP_AT=<hex> DUMP_LEN=<n> at t=DUMP_T (default 26)
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local a, n, tt = tonumber(os.getenv("DUMP_AT"), 16), tonumber(os.getenv("DUMP_LEN") or "64"), tonumber(os.getenv("DUMP_T") or "26")
sub = emu.add_machine_frame_notifier(function()
  if manager.machine.time:as_double() > tt then
    local s = {}
    for i = 0, n - 1 do s[#s + 1] = string.format("%02x", mem:read_u8(a + i)) end
    print("DUMP " .. table.concat(s))
    manager.machine:exit()
  end
end)

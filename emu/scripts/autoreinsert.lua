-- Reinsert the boot floppy after the Mac ejects it (Restart and Shut Down eject
-- floppies, which would otherwise leave the emulator on the "?" disk screen).
local disk = os.getenv("DISK")
if disk == "" then disk = nil end   -- hard-disk-only game
local floppy
for tag, img in pairs(manager.machine.images) do
  if tag:find("fdc:0") then floppy = img end
end
local gone_at
sub = emu.add_machine_frame_notifier(function()
  if not floppy or not disk then return end
  if floppy.exists then gone_at = nil; return end
  local t = manager.machine.time:as_double()
  if not gone_at then
    gone_at = t
  elseif t - gone_at > 2 then
    floppy:load(disk)
    gone_at = nil
  end
end)

-- Automate Apple's Installer (System 7.0.1, Easy Install) on macsefd, onto the SCSI disk.
-- Started by install_system.sh: boots from "Install 1"; $INSTALL_DIR holds writable copies of
-- the six install floppies. Whenever the drive has been empty for 1.5 s, the next disk in the
-- cycle is inserted (a wrong one is ejected again by the Installer). Screenshots every 30 s.
dofile(os.getenv("EMU") .. "/../toolchain/se7/se_lib.lua")
local dir = os.getenv("INSTALL_DIR")
local cycle = {"Install 2", "Tidbits", "Printing", "Fonts", "Install 1", "Disk Tools"}
local nexti, empty_since, started, inserts, last_insert = 1, nil, false, 0, 0
local finish_at = nil      -- emulated time when the success alert was seen
local function click(h, v, t0)
  return {
    {t0, function() mouse_to(h, v) end},
    {t0 + 0.1, function() btn:set_value(1) end},
    {t0 + 0.25, function() btn:clear_value() end},
  }
end
local steps = {}
for _, s in ipairs(click(409, 296, 110)) do steps[#steps + 1] = s end      -- Welcome: OK
for _, s in ipairs(click(423, 239, 125)) do steps[#steps + 1] = s end      -- Switch Disk -> Macintosh HD
for _, s in ipairs(click(424, 122, 132)) do steps[#steps + 1] = s end      -- Install
steps[#steps + 1] = {133, function() started = true end}
run_steps(steps)

local function px(h, v)
  local base = mem:read_u32(0x824)
  return (mem:read_u8(base + v * 64 + (h >> 3)) >> (7 - (h & 7))) & 1
end
function success_dialog()
  for h = 350, 425 do if px(h, 223) == 0 then return false end end
  return px(344, 235) == 1 and px(345, 235) == 1 and px(346, 235) == 1 and px(347, 235) == 0 and px(348, 235) == 1
end
local fd = floppy(0)
local next_snap = 150
sub2 = emu.add_machine_frame_notifier(function()
  local t = manager.machine.time:as_double()
  if t >= next_snap then
    screen:snapshot(string.format("inst_%04d.png", math.floor(t))); next_snap = t + (finish_at and 10 or 30)
  end
  if not started then return end
  -- "Installation on Macintosh HD was successful" alert: recognise its bold Quit button
  -- (row 223 black from x=350 to 425, triple border at x=344..346 on row 235), click Quit.
  if not finish_at and inserts >= 4 and success_dialog() then
    finish_at = t
    print(string.format("t=%.0f installation successful; clicking Quit", t))
    screen:snapshot("success.png")
    mouse_to(388, 235); btn:set_value(1)
  end
  if finish_at then
    if t > finish_at + 0.2 and t < finish_at + 1 then btn:clear_value() end
    -- Quit shuts the Mac down cleanly ("It is now safe to switch off your Macintosh"), so the
    -- volume is flushed. (Pressing Restart there hangs MAME's SE, so stop here; the first boot
    -- from the hard disk is a separate run.)
    if t > finish_at + 20 then
      print(string.format("t=%.0f shut down; stopping", t)); screen:snapshot("shutdown.png"); manager.machine:exit()
    end
    return
  end
  if fd.exists then empty_since = nil; return end
  if not empty_since then empty_since = t; return end
  if t - empty_since > 1.5 then
    local name = cycle[nexti]; nexti = nexti % #cycle + 1
    inserts = inserts + 1
    print(string.format("t=%.0f insert %s (%d)", t, name, inserts))
    fd:load(dir .. "/" .. name .. ".img")
    empty_since = nil; last_insert = t
  end
end)

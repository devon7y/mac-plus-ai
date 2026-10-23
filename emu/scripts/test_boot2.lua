dofile(os.getenv("EMU") .. "/scripts/lib.lua")
run_steps({
  { 32, function() globals("cold boot"); screen:snapshot("iso_reboot.png") end },
  { 32.1, function() manager.machine:exit() end },
})

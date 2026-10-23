dofile(os.getenv("EMU") .. "/scripts/lib.lua")
run_steps({
  { 30, function() globals("desktop"); screen:snapshot("real_desktop.png") end },
  { 31, function() manager.machine:exit() end },
})

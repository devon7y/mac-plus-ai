dofile(os.getenv("EMU") .. "/scripts/lib.lua")
run_steps({ { 35, function() manager.machine:exit() end } })

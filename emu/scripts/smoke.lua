-- Smoke test: boot, report app/memory, screenshot, exit.
dofile(os.getenv("EMU") .. "/scripts/lib.lua")
run_steps({ { tonumber(os.getenv("SMOKE_AT") or "35"), function() globals("smoke " .. (os.getenv("RAM") or "1M")); screen:snapshot("smoke.png"); manager.machine:exit() end } })

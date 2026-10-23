-- SE floppy test: Finder double-clicks (se_launch.lua) + Pixel's debug block, swap and tokens (pixel_test.lua).
local dir = os.getenv("LLM_APP")
dofile(dir .. "/se_launch.lua")
dofile(dir .. "/pixel_test.lua")

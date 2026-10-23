dofile(os.getenv("EMU") .. "/scripts/lib.lua")
local base = 0xFA700
local function fb(row) return mem:read_u32(base + row * 64) end
local function trace(tag)
  local im = fb(301)
  local function rc(a, b) local x, y = fb(a), fb(b); return string.format("(%d,%d,%d,%d)", (x>>16)~0, x&0xFFFF, (y>>16), y&0xFFFF) end; local lr = fb(304); print(string.format("[%5s] entries=%d last item=%d msg=%d stage=%d part=%d left=%d right=%d", tag, fb(300), (im >> 16) & 0xFFFF, im & 0xFFFF, fb(302), fb(303), (lr >> 16) & 0xFFFF, lr & 0xFFFF)); print("   clip=" .. rc(305, 306) .. " vis=" .. rc(307, 308))
end
local steps = {
  { 28.0, function() mouse_to(12, 8) end }, { 28.3, function() btn:set_value(1) end },
  { 28.8, function() mouse_to(60, 107) end }, { 29.3, function() btn:clear_value() end },
  { 40.0, function() for r = 300, 308 do mem:write_u32(base + r * 64, 0) end end },
  { 45.0, function() mouse_to(98, 258) end }, { 45.2, function() btn:set_value(1) end }, { 45.4, function() btn:clear_value() end },
  { 55.0, function() trace("open") end },
  { 56.0, function() mouse_to(325, 130) end },
  { 56.2, function() btn:set_value(1) end },
  { 56.4, function() btn:clear_value() end },
}
for _, dt in ipairs({ 0.2, 0.6, 1.5, 4.0 }) do table.insert(steps, { 56.4 + dt, function() trace(string.format("+%.1f", dt)); screen:snapshot(string.format("fb_%03.1f.png", dt)) end }) end
table.insert(steps, { 62, function() screen:snapshot("fb_after.png"); manager.machine:exit() end })
run_steps(steps)

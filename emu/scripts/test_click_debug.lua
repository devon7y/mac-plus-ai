dofile(os.getenv("EMU") .. "/scripts/lib.lua")
local dbg = manager.machine.debugger
local seen = 0
local function flush()
  local log = dbg.consolelog
  for i = seen + 1, #log do print("DBG: " .. log[i]) end
  seen = #log
end
local armed = false
local steps = {
  { 28.0, function() mouse_to(12, 8) end }, { 28.3, function() btn:set_value(1) end },
  { 28.8, function() mouse_to(60, 107) end }, { 29.3, function() btn:clear_value() end },
  { 45.0, function() mouse_to(98, 258) end }, { 45.2, function() btn:set_value(1) end }, { 45.4, function() btn:clear_value() end },
  { 55.0, function()
      -- arm only now, so boot-time probes don't trigger it
      local v4, v3, v2 = mem:read_u32(0x10), mem:read_u32(0x0C), mem:read_u32(0x08)
      print(string.format("vectors: illegal=%06x addrerr=%06x buserr=%06x", v4, v3, v2))
      dbg:command(string.format('bpset %X,1,{printf "ILLEGAL frame: SR=%%04X PC=%%06X  a0=%%06X a1=%%06X a2=%%06X a4=%%06X a6=%%06X sp=%%06X",w@sp,d@(sp+2),a0,a1,a2,a4,a6,sp; history ,60; go}', v4))
      dbg:command(string.format('bpset %X,1,{printf "ADDRERR frame: PC=%%06X access=%%06X",d@(sp+10),d@(sp+2); history ,40; go}', v3))
      local syserr = mem:read_u32(0xC00 + 0x1C9*4)
      local gne = mem:read_u32(0xC00 + 0x170*4)
      print(string.format("SysError handler=%06x GetNextEvent=%06x", syserr, gne))
      dbg:command(string.format('bpset %X,1,{printf "SYSERROR d0=%%d sp=%%06X ret=%%06X",d0,sp,d@sp; history ,80; go}', syserr))
      dbg:command(string.format('bpset %X,1,{printf "GNE hit"; bpdisable 4; go}', gne))
      flush(); armed = true
    end },
  { 56.0, function() mouse_to(325, 130) end },
  { 56.2, function() btn:set_value(1) end },
  { 56.4, function() btn:clear_value() end },
  { 62.0, function() flush(); screen:snapshot("dbg_after.png"); manager.machine:exit() end },
}
run_steps(steps)

-- SLOW=<factor> for emu/run_test.sh: make the emulated 68000 do 1/factor of the work per frame,
-- to approximate a real Mac Plus or SE (see toolchain/README.md, "Real-hardware speed").
--
-- Why: MAME 0.289 runs the Plus/SE 68000 at the full 7.83 MHz. Its video/sound contention code
-- (src/mame/apple/mac128.cpp, mac_scanline/mac_hblank: adjust_icount(-128) / (-4)) is called from
-- timer callbacks, where adjust_icount does nothing (src/emu/diexec.h:159 only acts while the CPU
-- is executing). The Lua API has no CPU clock control, so the cycles are taken inside the Mac:
--
-- A 30-byte wrapper in the reserved 68000 vectors at $E0 is installed in front of the level-1
-- (VIA) interrupt handler (vector $64). On every vertical-blank interrupt (VIA IFR bit CA1 set)
-- it spins for (1 - 1/factor) of a frame's 130,234 CPU cycles, then jumps to the original
-- handler. Other VIA interrupts pass straight through. The vector is checked every frame and
-- re-installed if the ROM or System rewrites it. While interrupts are masked (e.g. during
-- floppy transfers) no cycles are taken, as no VBL interrupt is taken then either.
local factor = tonumber(os.getenv("SLOW") or "1")
if factor and factor > 1.0001 then
  local mem = manager.machine.devices[":maincpu"].spaces["program"]
  local VEC, CODE = 0x64, 0xE0
  local FRAME = 7833600 / 60.15                       -- CPU cycles per video frame
  local iters = math.floor(FRAME * (1 - 1 / factor) / 10 + 0.5)    -- dbra loop: 10 cycles each
  if iters > 65535 then iters = 65535 end
  local function code(orig)
    return {
      0x0839, 0x0001, 0x00EF, 0xFBFE,       -- btst   #1,$EFFBFE     (VIA IFR, CA1 = VBL)
      0x6700, 0x000E,                       -- beq.w  +14 -> jmp
      0x2F00,                               -- move.l d0,-(sp)
      0x303C, iters - 1,                    -- move.w #iters-1,d0
      0x51C8, 0xFFFE,                       -- dbra   d0,*
      0x201F,                               -- move.l (sp)+,d0
      0x4EF9, (orig >> 16) & 0xFFFF, orig & 0xFFFF,   -- jmp orig
    }
  end
  local orig, installs = nil, 0
  local function installed()
    if mem:read_u32(VEC) ~= CODE or not orig then return false end
    local c = code(orig)
    for i, w in ipairs(c) do if mem:read_u16(CODE + 2 * (i - 1)) ~= w then return false end end
    return true
  end
  slow_sub = emu.add_machine_frame_notifier(function()
    if installed() then return end
    local v = mem:read_u32(VEC)
    if v ~= CODE then orig = v end
    if not orig or orig == 0 or orig >= 0x1000000 then return end
    for i, w in ipairs(code(orig)) do mem:write_u16(CODE + 2 * (i - 1), w) end
    mem:write_u32(VEC, CODE)
    installs = installs + 1
    if installs <= 3 then
      print(string.format("SLOW %.2f: %d cycles taken per VBL (handler %06x), install #%d at t=%.1f",
        factor, iters * 10, orig, installs, manager.machine.time:as_double()))
    end
  end)
end

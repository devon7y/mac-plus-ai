-- M5 test: draw a prompt with the pencil, run Finish it through the mailbox (fixed category,
-- seed, temperature), then through the real UI (Finish it button, type to search, Return),
-- undo. Dumps the recorded points, the prompt rows the Mac fed and the rows it generated,
-- for check_m5.py to compare with paint/bench/ref.py.
dofile(os.getenv("PAINT_TESTS") .. "/dlib.lua")
local CAT, SEED, TEMP = tonumber(os.getenv("FI_CAT") or "57"), tonumber(os.getenv("FI_SEED") or "305419896"), 1
local function fd(x, y) local h, v = fxy(x, y); return { h, v } end
local function wait_fi(timeout)
  local t_end = now() + timeout
  while now() < t_end do
    local st = u32(88)
    if st >= 4 then return st end
    sleep(0.5)
  end
  return u32(88)
end
local function dump_run(tag, n)   -- n = points recorded before the run (the prompt)
  dump_mem(tag .. "_points", u32(80), n * 6)
  dump_mem(tag .. "_prompt", u32(156), u32(160) * 6)
  dump_mem(tag .. "_gen", u32(108), u32(104) * 6)
  local f = io.open(OUT .. "/" .. tag .. "_info.txt", "w")
  f:write(string.format("state %d cat %d seed %d primed %d gen %d nprompt %d npoints %d t0 %d t1 %d t2 %d model %d freemem %d load %d modelbytes %d where %d\n",
    u32(88), u32(92), u32(96), u32(100), u32(104), u32(160), n, u32(112), u32(116), u32(120), u32(124), u32(56), u32(164), u32(168), i16(174)))
  f:close()
  print(string.format("[%s] state %d cat %d seed %d primed %d gen %d prompt rows %d, priming %d ticks, drawing %d ticks, model %x, FreeMem %d",
    tag, u32(88), u32(92), u32(96), u32(100), u32(104), u32(160), u32(116) - u32(112), u32(120) - u32(116), u32(124), u32(56)))
end

run(function()
  if not find_block(90) then print("no debug block"); finish(false) return end
  check("event loop ready", wait_ready(60))
  layout()
  -- prompt: a rounded head and two ears (pencil, the default tool)
  drag({ fd(80, 120), fd(70, 150), fd(80, 180), fd(110, 200), fd(150, 200), fd(180, 180), fd(190, 150), fd(180, 120),
         fd(150, 100), fd(110, 100), fd(80, 120) }, 0.05)
  drag({ fd(90, 108), fd(85, 70), fd(115, 100) }, 0.05)
  drag({ fd(150, 100), fd(175, 70), fd(178, 112) }, 0.05)
  check("prompt recorded", u32(76) > 4, "points " .. u32(76))
  local before = black(0, 0, 336, 272)
  shot("m5_00_prompt")
  -- 1) mailbox: fixed category, seed, temperature; mouse button ignored (arg3 = 1)
  local npts = u32(76)
  w32(132, CAT); w32(136, SEED); w32(140, TEMP); w32(144, 1); w32(128, 1)
  local st = wait_fi(600)
  check("finish it (mailbox) completed", st == 4, "state " .. st)
  dump_run("mb", npts)
  shot("m5_01_mailbox_done")
  dump_doc("m5_after_mailbox")
  check("completion drew something", black(0, 0, 336, 272) > before, before .. " -> " .. black(0, 0, 336, 272))
  -- undo removes it
  cmd_key("z"); sleep(1)
  check("undo removes the completion", black(0, 0, 336, 272) == before)
  -- 2) the UI: seed for the next run, Finish it button, type "cat", Return
  check("undo also restores the prompt", u32(76) == npts, u32(76) .. " vs " .. npts)
  mailbox(2, SEED + 1)
  w32(144, 1)
  click(finish_xy()); sleep(2)
  shot("m5_02_picker")
  type_text("cat"); sleep(1)
  shot("m5_03_picker_cat")
  key("Return")
  st = wait_fi(600)
  check("finish it (UI) completed", st == 4, "state " .. st)
  dump_run("ui", npts)
  shot("m5_04_ui_done")
  print(string.format("CHECKS %d, FAILED %d", CHECKS, FAILED))
  finish(FAILED == 0)
end)

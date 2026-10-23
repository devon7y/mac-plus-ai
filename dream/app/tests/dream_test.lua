-- Dream app test (MAME): load the model, dream through the mailbox (fixed subject and seed) and
-- dump the final x0 for check.py (bit-exact against engine/picture.py), then the UI: type a
-- query, Return, stop with a click after the first step; Copy and Save as MacPaint.
-- Env: DREAM_TESTS (this folder), SUBJ, SEED, QUERY, LOADWAIT (emulated s), UI=0 to skip the UI part.
dofile(os.getenv("DREAM_TESTS") .. "/dlib.lua")
local SUBJ, SEED = tonumber(os.getenv("SUBJ") or "3"), tonumber(os.getenv("SEED") or "12345")
local LOADWAIT = tonumber(os.getenv("LOADWAIT") or "400")
local function wait_state(pred, timeout)
  local t_end = now() + timeout
  while now() < t_end do
    if pred(u32(12)) then return true end
    sleep(0.5)
  end
  return false
end
local function info(tag)
  local s = string.format("[%s] state %d err %d load %d ticks freemem %d modelram %d code %d subject %d seed %d steps %d/%d begin %d ticks picsum %u steps(ticks):",
    tag, u32(12), u32(16), u32(20), u32(24), u32(28), u32(32), u32(36), u32(40), u32(44), u32(48), u32(156), u32(168))
  for i = 0, u32(48) - 1 do s = s .. " " .. u32(60 + 4 * i) end
  s = s .. string.format(" | read %d ticks, compile span %d ticks", u32(212), u32(216))
  print(s)
  local f = io.open(OUT .. "/" .. tag .. "_info.txt", "w"); f:write(s .. "\n"); f:close()
end

run(function()
  if not find_block(150) then print("no debug block"); finish(false) return end
  check("model loaded", wait_state(function(s) return s >= 2 end, LOADWAIT) and u32(12) == 2,
        "state " .. u32(12) .. " err " .. u32(16))
  info("load")
  shot("01_ready")
  if u32(12) ~= 2 then finish(false) return end
  -- 1) mailbox dream: fixed subject and seed
  w32(128, SUBJ); w32(132, SEED); w32(124, 1)
  check("dream started", wait_state(function(s) return s == 3 end, 30))
  sleep(5); shot("02_dreaming")
  check("dream finished", wait_state(function(s) return s == 4 end, 1200), "state " .. u32(12))
  info("mailbox")
  dump_mem("x0_mailbox", u32(52), u32(56) * 2)
  shot("03_done")
  if os.getenv("UI") == "0" then print(string.format("CHECKS %d, FAILED %d", CHECKS, FAILED)); finish(FAILED == 0) return end
  -- 2) UI: type a query, Return; stop with a click on the picture after the first step
  type_text(os.getenv("QUERY") or "sub"); sleep(1)
  shot("04_query")
  local before = u32(152)
  key("Return")
  check("UI dream started", wait_state(function(s) return s == 3 end, 30))
  local t_end = now() + 120
  while now() < t_end and u32(44) < 1 do sleep(0.5) end
  local pr = { top = i16(172), left = i16(174), bottom = i16(176), right = i16(178) }
  click((pr.left + pr.right) // 2, (pr.top + pr.bottom) // 2)
  check("click stops after the current step", wait_state(function(s) return s == 6 end, 120) and u32(44) <= 2,
        "state " .. u32(12) .. " steps " .. u32(44))
  info("ui_stopped")
  shot("05_stopped")
  -- 3) Copy (mailbox 3) and Save as MacPaint (mailbox 2 -> "Test1")
  local d = u32(144)
  w32(124, 3); sleep(2)
  check("copy put a PICT on the scrap", u32(144) > d and u32(160) > 0, "scrap " .. u32(160))
  d = u32(144)
  w32(128, 1); w32(124, 2); sleep(4)
  check("save as MacPaint", u32(144) > d and u32(148) == 0 and u32(164) >= 1, "result " .. u32(148))
  shot("06_end")
  -- 4) empty search box + Return = Surprise me (a random subject, not the first list entry)
  for _ = 1, #(os.getenv("QUERY") or "sub") do key("Backspace") end
  sleep(1)
  key("Return")
  check("empty query dreams", wait_state(function(s) return s == 3 end, 30))
  check("empty query is a surprise (not subject 0)", u32(36) ~= 0, "subject " .. u32(36))
  info("surprise")
  local t2 = now() + 120
  while now() < t2 and u32(44) < 1 do sleep(0.5) end
  click((pr.left + pr.right) // 2, (pr.top + pr.bottom) // 2)
  check("surprise stopped by a click", wait_state(function(s) return s == 6 end, 120))
  shot("07_surprise")
  print(string.format("CHECKS %d, FAILED %d", CHECKS, FAILED))
  finish(FAILED == 0)
end)

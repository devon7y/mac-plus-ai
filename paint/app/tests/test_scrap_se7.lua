-- System 7 (SE): Doodle's Copy goes to the system scrap as a 'PICT' that the Scrapbook accepts,
-- and a picture copied from the Scrapbook pastes back into Doodle bit for bit.
dofile(os.getenv("PAINT_TESTS") .. "/dlib.lua")
local function fd(x, y) local h, v = fxy(x, y); return { h, v } end
local EXPLORE = os.getenv("EXPLORE")

run(function()
  if not find_block(180) then print("no debug block"); finish(false) return end
  check("event loop ready", wait_ready(60))
  layout(); sleep(4)
  -- a filled black oval, then select it
  click(tool_xy(11)); drag({ fd(60, 60), fd(100, 90), fd(140, 120) })
  click(tool_xy(0)); drag({ fd(55, 55), fd(100, 90), fd(146, 126) })
  local sel = { top = i16(66), left = i16(68), bottom = i16(70), right = i16(72) }
  local ovalN = black(sel.left, sel.top, sel.right, sel.bottom)
  check("oval drawn and selected", i16(64) == 1 and ovalN > 1000, "black " .. ovalN)
  cmd_key("c")
  check("copy: PICT on the scrap", u32(176) > 100, u32(176) .. " bytes")
  -- Scrapbook from the Apple menu (item SCRAP_ITEM), paste into it
  local y = tonumber(os.getenv("SCRAP_Y") or "186")       -- Scrapbook in the System 7.0.1 Easy Install Apple menu
  menu_pick_y(menu_x(1), y, "s7_00_apple_menu")
  sleep(4); shot("s7_01_scrapbook")
  cmd_key("v"); sleep(3); shot("s7_02_scrapbook_pasted")
  if EXPLORE then finish(true) return end
  -- back to Doodle, and empty the system scrap (test mailbox), so only the Scrapbook can bring the oval back
  click(fxy(200, 230)); sleep(2); shot("s7_03_back")
  mailbox(6)
  check("scrap emptied", mailbox(7) == 0)
  -- Scrapbook again: copy the oval page, close it
  menu_pick_y(menu_x(1), y); sleep(4)
  for i = 1, 8 do click(82, 267); sleep(0.3) end        -- the Scrapbook's left arrow: back to page 1
  sleep(1)
  cmd_key("c"); sleep(1); shot("s7_04_scrapbook_copy")
  cmd_key("w"); sleep(3); shot("s7_05_closed")
  click(fxy(200, 230)); sleep(2)
  -- paste in Doodle: the oval comes back (centred), bit for bit
  click(fxy(10, 240)); sleep(1)
  local ops0 = u32(48); cmd_key("v"); wait_ops(ops0, 1, 10)
  local p = { top = i16(66), left = i16(68), bottom = i16(70), right = i16(72) }
  local back = black(p.left, p.top, p.right, p.bottom)
  check("Scrapbook put a PICT on the scrap", u32(176) > 100, "PICT " .. u32(176) .. " bytes")
  check("same oval came back", back == ovalN and (p.right - p.left) == (sel.right - sel.left), ovalN .. " vs " .. back)
  shot("s7_06_pasted_back")
  print(string.format("CHECKS %d, FAILED %d", CHECKS, FAILED))
  finish(FAILED == 0)
end)

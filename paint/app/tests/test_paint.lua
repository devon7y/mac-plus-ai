-- M2 test: every tool through the real palette, undo/redo, selection move and clear, text,
-- clearing with a double-click on the eraser, save/open through the mailbox and through the
-- Standard File dialogs. Prints CHECK lines and RESULT PASS/FAIL; dumps the page to $TEST_OUT.
dofile(os.getenv("PAINT_TESTS") .. "/dlib.lua")

local T = { SELECT = 0, TEXT = 1, BUCKET = 2, SPRAY = 3, BRUSH = 4, PENCIL = 5, LINE = 6, ERASER = 7,
            RECT = 8, FRECT = 9, OVAL = 10, FOVAL = 11 }
local function tool(t) click(tool_xy(t)); sleep(0.2) end
local function pat(i) click(pat_xy(i)) end
local function fd(x, y) local h, v = fxy(x, y); return { h, v } end      -- frame point -> screen
local function fblack(x0, y0, x1, y1)                                  -- frame rect, black count
  return black(x0 + 40, y0 + 8, x1 + 40, y1 + 8)
end

run(function()
  if not find_block(90) then print("no debug block"); finish(false) return end
  print(string.format("block at %06x", B))
  check("event loop ready", wait_ready(60))
  layout()
  print(string.format("canvas l%d t%d r%d b%d, frame l%d t%d", L.canvas.left, L.canvas.top, L.canvas.right,
    L.canvas.bottom, L.frame.left, L.frame.top))
  sleep(tonumber(os.getenv("SETTLE") or "1")); shot("m2_00_start")   -- (System 7: the Finder takes ~1 s once after launch)

  -- pencil (default tool): zigzag; it is also recorded as the AI prompt
  drag({ fd(20, 20), fd(40, 40), fd(60, 60), fd(80, 40), fd(100, 20), fd(120, 40), fd(140, 60) }, 0.06)
  check("pencil draws", fblack(15, 15, 145, 65) > 100, fblack(15, 15, 145, 65))
  check("pencil stroke recorded as prompt", u32(76) >= 7, "points " .. u32(76))
  -- line, width index 1 (2 px)
  tool(T.LINE); drag({ fd(20, 100), fd(70, 100), fd(120, 100) })
  check("line", fblack(18, 98, 124, 104) >= 180, fblack(18, 98, 124, 104))
  -- rectangle outline, filled rectangle (50% gray), oval, filled oval (black)
  tool(T.RECT); drag({ fd(20, 120), fd(80, 170) })
  local rb = fblack(18, 118, 84, 174)
  check("rectangle outline", rb > 200 and rb < 600, rb)
  tool(T.FRECT); pat(3); drag({ fd(90, 120), fd(140, 170) })
  local fr = fblack(90, 120, 141, 171)
  check("filled rectangle with gray", fr > 1200 and fr < 1800, fr)
  tool(T.OVAL); pat(0); drag({ fd(150, 120), fd(200, 170) })
  local ov = fblack(148, 118, 204, 174)
  check("oval outline", ov > 200 and ov < 600, ov)
  tool(T.FOVAL); drag({ fd(210, 120), fd(250, 170) })
  local fo = fblack(208, 118, 254, 174)
  check("filled oval", fo > 1300, fo)
  sleep(0.5); shot("m2_01_shapes")
  -- paint bucket with vertical lines inside the rectangle outline
  local before = fblack(24, 124, 76, 166)
  tool(T.BUCKET); pat(7)
  local ops0 = u32(48); click(fxy(50, 145)); wait_ops(ops0)
  local after = fblack(24, 124, 76, 166)
  check("bucket fills the rectangle", after > before + 400, before .. " -> " .. after)
  check("bucket stays inside", fblack(0, 180, 18, 255) == 0)
  print("bucket fill took " .. i16(74) .. " ticks")
  shot("m2_01b_bucket"); dump_doc("m2_bucket")
  -- spray can (black), brush (8 px, black)
  tool(T.SPRAY); pat(0); local sx, sy = fxy(40, 215); hold(sx, sy, 0.7)
  local sp = fblack(30, 205, 51, 226)
  check("spray can", sp > 10 and sp < 300, sp)
  tool(T.BRUSH); click(size_xy(2)); drag({ fd(100, 200), fd(130, 215), fd(160, 230) })
  check("brush", fblack(95, 192, 172, 240) > 300, fblack(95, 192, 172, 240))
  -- eraser across the line
  local lineb = fblack(55, 95, 90, 106)
  tool(T.ERASER); drag({ fd(60, 100), fd(70, 100), fd(80, 100) })
  check("eraser", fblack(55, 95, 90, 106) < lineb - 30, lineb .. " -> " .. fblack(55, 95, 90, 106))
  -- text
  tool(T.TEXT); click(fxy(185, 200)); type_text("Hi!"); key("Return"); type_text("Ok")
  local tx = fblack(183, 190, 230, 225)
  check("text typed", tx > 30, tx)
  tool(T.PENCIL)
  sleep(0.5); shot("m2_02_all_tools")
  dump_doc("m2_all_tools")

  -- selection: select the oval and move it 50 px down
  tool(T.SELECT); drag({ fd(146, 116), fd(175, 145), fd(205, 175) })
  check("selection made", i16(64) == 1, string.format("sel l%d t%d r%d b%d", i16(68), i16(66), i16(72), i16(70)))
  local old0, old0top = fblack(148, 118, 204, 174), fblack(148, 118, 204, 166)
  drag({ fd(175, 145), fd(175, 165), fd(175, 185), fd(175, 195) })
  local oldA, newA = fblack(148, 118, 204, 166), fblack(148, 168, 204, 224)
  check("selection moved", oldA < 5 and newA > 200, old0top .. " -> old " .. oldA .. ", new " .. newA)
  -- undo and redo
  cmd_key("z")
  check("undo restores", fblack(148, 118, 204, 174) == old0, fblack(148, 118, 204, 174))
  cmd_key("z")
  check("redo (undo again)", fblack(148, 118, 204, 166) == oldA)
  -- select the moved oval and clear it with Backspace
  drag({ fd(146, 166), fd(205, 226) })
  key("Backspace")
  check("backspace clears the selection", fblack(148, 168, 204, 224) < 5, fblack(148, 168, 204, 224))
  tool(T.PENCIL)
  sleep(0.3); shot("m2_03_selection")

  -- Cut / Copy / Paste through the system scrap ('PICT'), as the Edit menu's Cmd-X/C/V
  tool(T.SELECT); drag({ fd(207, 117), fd(230, 140), fd(255, 174) })     -- the filled oval
  local ovalN = fblack(207, 117, 255, 174)
  local clip0 = u32(180)
  cmd_key("c")
  check("copy puts a PICT on the scrap", u32(180) == clip0 + 1 and u32(176) > 100, "PICT " .. u32(176) .. " bytes")
  click(fxy(10, 240))                                                     -- deselect
  local ops0 = u32(48); cmd_key("v"); wait_ops(ops0)
  local sel = { top = i16(66), left = i16(68), bottom = i16(70), right = i16(72) }   -- Rect: t, l, b, r
  check("paste makes a selection", i16(64) == 1, string.format("l%d t%d r%d b%d", sel.left, sel.top, sel.right, sel.bottom))
  local pasted = black(sel.left, sel.top, sel.right, sel.bottom)
  check("pasted bits equal the copied oval", pasted == ovalN, ovalN .. " vs " .. pasted)
  check("paste is centred in the canvas", math.abs((sel.left + sel.right) - 336) <= 1 and math.abs((sel.top + sel.bottom) - 272) <= 1)
  shot("m2_03b_pasted")
  ops0 = u32(48); cmd_key("x"); wait_ops(ops0)
  check("cut clears the selection", black(sel.left, sel.top, sel.right, sel.bottom) == 0)
  ops0 = u32(48); cmd_key("v"); wait_ops(ops0)
  check("paste again after cut", black(sel.left, sel.top, sel.right, sel.bottom) == ovalN)
  cmd_key("z")
  check("undo removes the paste", black(sel.left, sel.top, sel.right, sel.bottom) == 0)
  click(fxy(10, 240))
  tool(T.PENCIL)

  -- temperature: default Normal (0.5); the Finish menu offers Tidy / Normal / Wild
  check("default temperature is 0.5 (normal)", i16(172) == 1, "temp index " .. i16(172))
  menu_pick(menu_x(4), 5, "m2_03c_finish_menu")
  check("menu: Wild (0.8)", i16(172) == 2, "temp index " .. i16(172))
  menu_pick(menu_x(4), 3)
  check("menu: Tidy (0.3)", i16(172) == 0, "temp index " .. i16(172))
  menu_pick(menu_x(4), 4)
  check("menu: back to Normal (0.5)", i16(172) == 1)
  -- About box (Apple menu, first item)
  w32(152, 0)
  menu_pick(menu_x(1), 1); sleep(0.5); shot("m2_03d_about")
  check("About box shown", u32(152) == 0xAB)
  click(fxy(10, 240))

  -- save and reopen through the mailbox
  dump_doc("m2_saved")
  local total = black(0, 0, 336, 272)
  check("mailbox save Test1", mailbox(3, 1) == 0)
  -- clear by double-clicking the eraser
  click(tool_xy(T.ERASER)); dclick(tool_xy(T.ERASER))
  check("double-click eraser clears", black(0, 0, 336, 272) == 0, black(0, 0, 336, 272))
  check("clear also clears the prompt", u32(76) == 0)
  check("mailbox open Test1", mailbox(4, 1) == 0)
  check("reopened page matches", black(0, 0, 336, 272) == total, total .. " vs " .. black(0, 0, 336, 272))
  dump_doc("m2_reopened")

  -- Save As through the Standard File dialog (no file name yet -> Save asks)
  local ops = u32(84)
  cmd_key("s"); sleep(2); shot("m2_04_save_dialog")
  -- the name field starts selected: typing replaces "Untitled"
  type_text("Dlg"); key("Return"); sleep(3)
  check("save dialog wrote the file", u32(84) == ops + 1 and u32(52) == 0, "err " .. u32(52))
  cmd_key("n"); sleep(1)
  check("new clears", black(0, 0, 336, 272) == 0)
  cmd_key("o"); sleep(2); shot("m2_05_open_dialog")
  key("Return"); sleep(3)
  check("open dialog read the file back", black(0, 0, 336, 272) == total, black(0, 0, 336, 272))
  sleep(0.5); shot("m2_06_end")
  -- a MacPaint file made from a PNG on the MacBook (Test7, in the System Folder): open, dump, save as Test8
  if not os.getenv("NO_FIXTURE") then
    check("open the PNG-converted file", mailbox(4, 7) == 0)
    dump_doc("m2_png_in"); shot("m2_07_png_in")
    check("save it again as Test8", mailbox(3, 8) == 0)
  end
  print(string.format("ops %d, fileOps %d, freeMem %d", u32(48), u32(84), u32(56)))
  print(string.format("CHECKS %d, FAILED %d", CHECKS, FAILED))
  finish(FAILED == 0)
end)

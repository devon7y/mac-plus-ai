# toolchain/lib: shared helpers for Plus / SE games (`plus.h`)

Small C library (works from C++) for the three things every game here needs. Verified in
MAME on the Mac Plus (System 6.0, Narrow Screen 448 px) and the SE (System 7.0.1, 512 px).

| Part | Functions |
| --- | --- |
| Screen | `pl_view_init(&view, w, h)` centres a w×h view in `qd.screenBits.bounds`; `pl_present(&view, buf, rowBytes)` copies a 1-bit offscreen buffer into it (`pl_present_rows` for a band of rows) |
| Input | `pl_input_update(&in)` once per frame: `in.held`, `in.pressed`, `in.released` as `PL_*` bits (`pl_input_read()` for a one-off read) |
| Pacing | `pl_pace_init(period)` (ticks per frame: 1 = 60 Hz, 2 = 30 Hz, 3 = 20 Hz); `pl_pace_wait()` at the top of each frame; `pl_ticks()` reads `Ticks` without a trap |

Keys (DECISIONS.md): arrows = `PL_UP/DOWN/LEFT/RIGHT`, X = `PL_A`, Z = `PL_B`,
Return = `PL_START`, Tab = `PL_SELECT`, Space = `PL_SPECIAL`, Command-Q = `PL_QUIT`,
Esc = `PL_ESC` (ADB keyboards only; the Plus keyboard has no Esc). Both keyboards' arrow
codes are mapped: the Plus keyboard sends `$4D/$48/$46/$42`, ADB keyboards `$7E/$7D/$7B/$7C`.

## Use

```cmake
# in your project's CMakeLists.txt (path relative to it)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../toolchain/lib ${CMAKE_BINARY_DIR}/pluslib)
target_link_libraries(MyGame PRIVATE plus)
```

```c
#include "plus.h"
static unsigned char buf[48 * 288];          /* 384 x 288, 48 bytes per row */
PlView view; PlInput in = {0};
/* after the usual InitGraf ... InitCursor: */
HideCursor();                                /* pl_present writes the screen directly */
pl_view_init(&view, 384, 288);
pl_pace_init(2);                             /* 30 Hz */
for (;;) {
    pl_pace_wait();
    pl_input_update(&in);
    if (in.held & PL_QUIT) break;
    /* ... update and draw into buf ... */
    pl_present(&view, buf, 48);
    /* every few frames: SystemTask(); GetNextEvent(...) so the system keeps running */
}
```

Rules: the view width must be a multiple of 16 px, and its left edge is rounded down to a
multiple of 16 (the 68000 only allows word and long moves at even addresses), so the view
is centred to within 8 px. The buffer's rowBytes must be even. Hide the cursor first.

## Measured (MAME, 2026-10-16)

| Test | Plus 1 MB, System 6 | SE 4 MB, System 7.0.1 |
| --- | --- | --- |
| `pl_present` 384×288 | **10.3 ms** (60 in 37 ticks) | 10.3 ms |
| `CopyBits` of the same buffer and place | 20.0 ms | 20.2 ms |
| `pl_present` 336×288 (rows end in a word) | 9.1 ms (`CopyBits` 18.8 ms) | not run |
| Screen equals buffer after a present | all 13,824 bytes (336 px: 12,096) | all 13,824 bytes |
| 60 frames at 30 Hz, idle / presenting each frame | 120 / 120 ticks, 0 late | 120 / 120 ticks, 0 late |
| Every mapped key seen in `held` (real key presses from Lua) | 9 keys + Command-Q | 10 keys + Command-Q |

`pl_present` jumps into an unrolled run of `move.l (a0)+,(a1)+` (inline asm), which is about
5 cycles per byte: 10.3 ms matches the 68000's cycle count for 13,824 bytes. MAME runs the CPU
without the Plus's video contention (see "Real-hardware speed" in `toolchain/README.md`):
with `SLOW=1.5`, the estimate for a real Plus, it takes **15.2 ms**, and `CopyBits` 30.8 ms.
At 30 Hz (33 ms per frame) a full 384×288 present then leaves about half the frame for the game.

Under **System 7 with the Finder running**, calling `GetNextEvent`/`WaitNextEvent` can hand
the Finder a long time slice: in the test the app stalled for about 1 s once, a few seconds
after it was launched from Startup Items (the Finder finishing its startup work). No stall
without the Finder (`--shell` disks) or on the Plus. `pl_pace_wait` resynchronises after
such a stall (it returns how many ticks late it was).

## Test

`test/main.c` (app) and `test/check_lib.lua` (MAME checker). `toolchain/build_all.sh` builds
`libtest_narrow.img` (Plus) and `libtest_se7.hd` (SE); then:

```sh
CHECK_AT=20 emu/run_test.sh toolchain/build/libtest_narrow.img toolchain/lib/test/check_lib.lua 250
MACHINE=macsefd RAM=4M HARDDISK=toolchain/build/libtest_se7.hd CHECK_AT=50 \
  emu/run_test.sh none toolchain/lib/test/check_lib.lua 300
```

The app shows a test pattern and waits until the Lua has compared the screen with the
buffer byte for byte (the Lua writes an acknowledgement into the app's 'PLIB' block), then
times 60 presents against 60 `CopyBits`, checks pacing, and runs a 30 Hz input loop; the
Lua presses every key and checks `held` (waiting until the app has run 6 frames after each
press). Build with `-DCMAKE_C_FLAGS=-DW=336` to test a width that ends in a word.

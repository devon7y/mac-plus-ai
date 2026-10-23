# Template Retro68 project for the Mac Plus (68000, System 6).
# Build (see toolchain/README.md):
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   cmake --build build
# Output: build/HelloPlus.bin (MacBinary application), plus .APPL/.dsk/.ad variants.
cmake_minimum_required(VERSION 3.13)
project(HelloPlus C)

# Name, type and creator of the application. The name is what make_game_disk.py writes
# into the boot blocks as the startup application (at most 15 characters).
add_application(HelloPlus
    main.c
    TYPE APPL
    CREATOR HPls
)

# -Os: the 68000 is slow, but code size matters more on an 800K floppy with 852K of RAM;
#      switch hot files to -O2 if profiling says so.
# -ffunction-sections + -gc-sections drop unused code; --mac-strip-macsbug drops the
# MacsBug function names (remove it while debugging).
target_compile_options(HelloPlus PRIVATE -Os -ffunction-sections -Wall -Wno-pointer-sign --param=min-pagesize=0)
set_target_properties(HelloPlus PROPERTIES
    LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug")

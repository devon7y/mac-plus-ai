# Shuttle for the Mac Plus / SE (Retro68). Build (shuttle/build.py does all of this):
#   cmake -S shuttle/app -B shuttle/build/app -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   cmake --build shuttle/build/app        -> Shuttle.bin (MacBinary)
cmake_minimum_required(VERSION 3.13)
project(Shuttle C)

add_application(Shuttle
    main.c proto.c link.c files.c serial.c shuttle.r
    TYPE APPL
    CREATOR Shtl
)
target_compile_options(Shuttle PRIVATE -Os -ffunction-sections -Wall -Wno-pointer-sign
    -Wno-discarded-qualifiers -Wno-multichar --param=min-pagesize=0)
set_target_properties(Shuttle PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug -Wl,--mac-single")

# Guess My Doodle engine benchmark (M0).
#   cmake -S . -B ../build/bench -G Ninja -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   cmake --build ../build/bench
cmake_minimum_required(VERSION 3.13)
project(GuessBench C ASM)
add_application(GuessBench main.c ../app/engine.c ../app/krun.s TYPE APPL CREATOR GsBm)
target_include_directories(GuessBench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../app)
target_compile_options(GuessBench PRIVATE -O2 -Wall -Wno-multichar -Wno-pointer-sign --param=min-pagesize=0)
set_target_properties(GuessBench PROPERTIES LINK_FLAGS "-Wl,--mac-strip-macsbug -Wl,--mac-single")

# Guess My Doodle (Retro68).
#   cmake -S . -B ../build/app -G Ninja -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   cmake --build ../build/app            -> ../build/app/GuessMyDoodle.bin
cmake_minimum_required(VERSION 3.13)
project(GuessMyDoodle C ASM)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_custom_command(OUTPUT ${GEN}/sounds.c
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_sounds.py ${GEN}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_sounds.py)
add_application(GuessMyDoodle main.c engine.c raster.c ink.c sound.c speech.c krun.s ${GEN}/sounds.c guess.r
    TYPE APPL CREATOR GsDd)
target_include_directories(GuessMyDoodle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(GuessMyDoodle PRIVATE -Os -ffunction-sections -Wall -Wno-multichar -Wno-pointer-sign
    --param=min-pagesize=0)
set_source_files_properties(engine.c raster.c PROPERTIES COMPILE_OPTIONS "-O2")
set_target_properties(GuessMyDoodle PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug -Wl,--mac-single")

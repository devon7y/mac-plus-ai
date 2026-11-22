# Broadside for the Mac Plus / SE (Retro68).
#   cmake -S . -B ../build/app -G Ninja -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   cmake --build ../build/app            -> ../build/app/Broadside.bin
cmake_minimum_required(VERSION 3.13)
project(Broadside C)

find_program(PYTHON3 NAMES python3)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_custom_command(
    OUTPUT ${GEN}/sounds.c
    COMMAND ${PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_sounds.py ${GEN}
    DEPENDS ../tools/gen_sounds.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_source_files_properties(${GEN}/sounds.c PROPERTIES GENERATED TRUE)

add_application(Broadside
    main.c board.c art.c fx.c sound.c speech.c stats.c ${GEN}/sounds.c
    ../src/ai.c ../src/game.c
    broadside.r
    TYPE APPL
    CREATOR BrDs
)
target_include_directories(Broadside PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(Broadside PRIVATE -Os -ffunction-sections -Wall -Wno-pointer-sign -Wno-multichar --param=min-pagesize=0)
# the AI is the hot code: -O2
set_source_files_properties(../src/ai.c ../src/game.c PROPERTIES COMPILE_OPTIONS "-O2")
set_target_properties(Broadside PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug")

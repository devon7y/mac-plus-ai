# Doodle: MacPaint-style paint program with "Finish it" for the Mac Plus (Retro68).
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   cmake --build build            -> build/Doodle.bin
#   python3 -I build_disk.py       -> build/doodle_disk.img
cmake_minimum_required(VERSION 3.13)
project(Doodle C ASM)

# The decoder kernels are generated from paint/bench/kernels.py (verified bit-exact in M1).
find_program(PYTHON3 NAMES python3 PATHS /opt/anaconda3/bin NO_DEFAULT_PATH)
find_program(PYTHON3 NAMES python3)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_custom_command(
    OUTPUT ${GEN}/kernels.s ${GEN}/kernels.h
    COMMAND ${PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/gen_kernels.py ${GEN}
    DEPENDS gen_kernels.py ../bench/kernels.py ../bench/tables.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_source_files_properties(${GEN}/kernels.s PROPERTIES GENERATED TRUE LANGUAGE ASM)
set_source_files_properties(model.c PROPERTIES OBJECT_DEPENDS ${GEN}/kernels.h)
include_directories(${GEN})

set(DOODLE_SOURCES main.c paint.c file.c finish.c model.c doodle.r ${GEN}/kernels.s)

add_application(Doodle
    ${DOODLE_SOURCES}
    TYPE APPL
    CREATOR Ddle
)
target_compile_options(Doodle PRIVATE -Os -ffunction-sections -Wall -Wno-pointer-sign -Wno-multichar --param=min-pagesize=0)
set_target_properties(Doodle PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug")

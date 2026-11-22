# TuneBench: speed of Tune's model on the Mac (M0/M1). Model: copy tune/build/<model>.bin to
# ${CMAKE_BINARY_DIR}/model.bin before building (bench/run_bench.sh does it).
cmake_minimum_required(VERSION 3.13)
project(TuneBench C ASM)
set(APP ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/kernels68k.s
    COMMAND python3 -I ${APP}/gen_kernels.py ${CMAKE_BINARY_DIR}/kernels68k.s
    DEPENDS ${APP}/gen_kernels.py)
set_source_files_properties(${CMAKE_BINARY_DIR}/kernels68k.s PROPERTIES GENERATED TRUE LANGUAGE ASM)
include_directories(${APP})
set(CMAKE_REZ_FLAGS "${CMAKE_REZ_FLAGS} -I${CMAKE_BINARY_DIR}")
add_application(TuneBench bench_main.c ${APP}/engine.c ${APP}/grammar.c ${APP}/tables.c
    ${CMAKE_BINARY_DIR}/kernels68k.s bench.r TYPE APPL CREATOR TunB)
target_compile_options(TuneBench PRIVATE -O2 -Wall -Wno-pointer-sign -Wno-multichar --param=min-pagesize=0)
set_target_properties(TuneBench PROPERTIES LINK_FLAGS "-Wl,--mac-strip-macsbug")

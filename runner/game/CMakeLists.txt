# Rail Runner (Retro68). Built by runner/build.py, which generates the data first:
#   cmake -S runner/game -B <build> -G Ninja -DCMAKE_TOOLCHAIN_FILE=... -DGEN_DIR=<gen> -DDEFS="BENCH"
cmake_minimum_required(VERSION 3.13)
project(RailRunner C ASM)
set(GEN_DIR "" CACHE PATH "folder with generated data.c, data.h, params.h")
set(DEFS "" CACHE STRING "extra compile definitions, ;-separated (BENCH, REPLAY)")
set(APPNAME "Rail Runner" CACHE STRING "application name (<= 15 chars)")

add_application(RailRunner
    main.c game.c render.c sound.c hiscore.c fast.S ${GEN_DIR}/data.c
    TYPE APPL
    CREATOR RRun
)
target_include_directories(RailRunner PRIVATE ${GEN_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
if(DEFS)
    target_compile_definitions(RailRunner PRIVATE ${DEFS})
endif()
target_compile_options(RailRunner PRIVATE -O2 -fomit-frame-pointer -ffunction-sections -Wall -Wno-pointer-sign)
set_target_properties(RailRunner PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug")

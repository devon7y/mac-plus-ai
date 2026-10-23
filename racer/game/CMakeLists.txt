# Endurance (Retro68). Built by racer/build.py, which generates the engine and data into GEN_DIR.
cmake_minimum_required(VERSION 3.13)
project(Endurance C ASM)
if(NOT GEN_DIR)
    message(FATAL_ERROR "set -DGEN_DIR=<racer/build/gen_game>")
endif()
add_application(Endurance
    main.c
    ${GEN_DIR}/road_engine.s
    ${GEN_DIR}/sprites.s
    ${GEN_DIR}/gamedata.s
    TYPE APPL
    CREATOR ENDR
)
target_include_directories(Endurance PRIVATE ${GEN_DIR})
target_compile_options(Endurance PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -g -Wall -Wno-pointer-sign -Wno-array-bounds>)
set_target_properties(Endurance PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-single")

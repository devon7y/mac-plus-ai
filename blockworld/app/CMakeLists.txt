# Blockworld (Retro68). Built by blockworld/build.py, which generates tables into GEN_DIR.
cmake_minimum_required(VERSION 3.13)
project(Blockworld C ASM)
if(NOT GEN_DIR)
    message(FATAL_ERROR "set -DGEN_DIR=<blockworld/build/gen>")
endif()
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../toolchain/lib ${CMAKE_BINARY_DIR}/pluslib)

set(ENGINE render.c cast.s raster.s segs.s unpack.s world.c ${GEN_DIR}/tables.c)

# M1 renderer benchmark
add_application(BWBench bench.c ${ENGINE} TYPE APPL CREATOR BWbn)
target_include_directories(BWBench PRIVATE ${GEN_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(BWBench PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -Wall -Wno-pointer-sign --param=min-pagesize=0>)
target_link_libraries(BWBench PRIVATE plus)
set_target_properties(BWBench PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-single")

# The game
add_application(Blockworld game.c player.c pick.c inv.c save.c lz.c lz.s obj.c mob.c drop.c sound.c sndvbl.s synth.c synth.s size.r ${GEN_DIR}/items.c ${GEN_DIR}/mobs_gen.c ${ENGINE} TYPE APPL CREATOR BWrd)
target_include_directories(Blockworld PRIVATE ${GEN_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(Blockworld PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -Wall -Wno-pointer-sign --param=min-pagesize=0>)
target_link_libraries(Blockworld PRIVATE plus)
set_target_properties(Blockworld PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-single")

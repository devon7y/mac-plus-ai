# Tank Duel (Retro68). Built by tanks/build.py, which generates the tables, art and policies first:
#   cmake -S tanks/app -B <build> -G Ninja -DCMAKE_TOOLCHAIN_FILE=... -DGEN_DIR=<gen>
cmake_minimum_required(VERSION 3.13)
project(TankDuel C ASM)
set(GEN_DIR "" CACHE PATH "folder with the generated art.c, policies.s, policies.h, polmap.h")
set(SIM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../sim)
set(LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../toolchain/lib)

add_application(TankDuel
    main.c render.c sound.c tankduel.r
    ${SIM_DIR}/sim.c ${SIM_DIR}/sim_tables.c
    ${GEN_DIR}/art.c ${GEN_DIR}/policies.s
    ${LIB_DIR}/plus.c
    TYPE APPL
    CREATOR TnkD
)
target_include_directories(TankDuel PRIVATE ${GEN_DIR} ${SIM_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${LIB_DIR})
target_compile_options(TankDuel PRIVATE -O2 -fomit-frame-pointer -ffunction-sections -Wall -Wno-pointer-sign
    --param=min-pagesize=0)
set_target_properties(TankDuel PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug")

# Orbit Duel (Retro68). Built by duel/build.py, which generates the art tables first:
#   cmake -S duel/app -B duel/build/app -G Ninja -DCMAKE_TOOLCHAIN_FILE=... -DGEN_DIR=duel/build/gen
cmake_minimum_required(VERSION 3.13)
project(OrbitDuel C ASM)
set(GEN_DIR "" CACHE PATH "folder with the generated art.c / art.h")

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../toolchain/lib ${CMAKE_BINARY_DIR}/pluslib)

add_application(OrbitDuel
    main.c sim.c render.c sound.c link.c serial.c atalk.c ${GEN_DIR}/art.c duel.r
    TYPE APPL
    CREATOR ODul
)
target_include_directories(OrbitDuel PRIVATE ${GEN_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(OrbitDuel PRIVATE plus)
target_compile_options(OrbitDuel PRIVATE -O2 -fomit-frame-pointer -ffunction-sections -Wall -Wno-pointer-sign
    --param=min-pagesize=0)
# one CODE segment: the AppleTalk socket listener runs at interrupt time and must never be unloaded
set_target_properties(OrbitDuel PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug -Wl,--mac-single")

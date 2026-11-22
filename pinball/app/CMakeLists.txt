# Neural Pinball (Retro68). Built by pinball/build.py, which generates the table data first:
#   cmake -S pinball/app -B <build> -G Ninja -DCMAKE_TOOLCHAIN_FILE=... -DGEN_DIR=<gen> [-DDEFS="HASHTEST"]
cmake_minimum_required(VERSION 3.13)
project(NeuralPinball C)
set(GEN_DIR "" CACHE PATH "folder with the generated table_data.c/.h")
set(DEFS "" CACHE STRING "extra compile definitions, ;-separated (HASHTEST, BENCH)")
set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../core)

add_application(NeuralPinball
    main.c render.c pbsound.c hiscore.c speech.c
    ${CORE}/phys.c ${CORE}/game.c ${CORE}/ai.c ${GEN_DIR}/table_data.c
    pinball.r
    TYPE APPL
    CREATOR NPbl
)
target_include_directories(NeuralPinball PRIVATE ${GEN_DIR} ${CORE} ${CMAKE_CURRENT_SOURCE_DIR})
if(DEFS)
    target_compile_definitions(NeuralPinball PRIVATE ${DEFS})
endif()
# -fno-tree-loop-distribute-patterns: keep small copy/fill loops inline instead of memcpy/memset calls
target_compile_options(NeuralPinball PRIVATE -O2 -fomit-frame-pointer -ffunction-sections -Wall
    -Wno-pointer-sign --param=min-pagesize=0 -fno-tree-loop-distribute-patterns)
set_target_properties(NeuralPinball PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug")

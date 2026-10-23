# Dream: a tiny diffusion model that paints clip art on the Mac Plus (Retro68).
#   cmake -S . -B ../build/app -G Ninja -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   cmake --build ../build/app     -> Dream.bin (4 MB: layers compiled to 68000 code)
#                                     DreamSm.bin (1 MB: lookup-table kernels; renamed "Dream" on its disk)
cmake_minimum_required(VERSION 3.13)
project(Dream C ASM)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_custom_command(OUTPUT ${GEN}/rtab.h ${GEN}/costab.h
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/gen_tables.py ${GEN}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen_tables.py)
add_custom_command(OUTPUT ${GEN}/lut68k.s
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/gen_lut.py ${GEN}/lut68k.s
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen_lut.py)
set_source_files_properties(${GEN}/lut68k.s PROPERTIES GENERATED TRUE LANGUAGE ASM)
set_source_files_properties(engine.c PROPERTIES OBJECT_DEPENDS "${GEN}/rtab.h;${GEN}/costab.h")
set(BENCH ${CMAKE_CURRENT_SOURCE_DIR}/../bench)
add_application(Dream main.c engine.c ${BENCH}/jit.c ${BENCH}/run.s dream.r TYPE APPL CREATOR Drem)
add_application(DreamSm main.c engine.c ${BENCH}/jit.c ${BENCH}/run.s ${GEN}/lut68k.s dream_small.r TYPE APPL CREATOR Drem)
target_compile_definitions(DreamSm PRIVATE DM_LUT)
foreach(t Dream DreamSm)
    target_include_directories(${t} PRIVATE ${GEN} ${BENCH} ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${t} PRIVATE -O2 -Wall -Wno-multichar -Wno-pointer-sign --param=min-pagesize=0)
    set_target_properties(${t} PROPERTIES LINK_FLAGS "-Wl,--mac-strip-macsbug -Wl,--mac-single")
endforeach()

# Dream kernel benchmark (M1). One app per network config (prog_<cfg>.h from gen_prog.py).
#   cmake -S . -B ../build/bench -G Ninja -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   cmake --build ../build/bench
cmake_minimum_required(VERSION 3.13)
project(DreamBench C ASM)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
foreach(cfg A64L A64L2 A64M A64M_q4 A64S A32 A32S)
    add_custom_command(OUTPUT ${GEN}/prog_${cfg}.h
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/gen_prog.py ${GEN} ${cfg}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen_prog.py ${CMAKE_CURRENT_SOURCE_DIR}/../engine/arch.py)
    add_application(DreamB${cfg} main.c jit.c run.s ${GEN}/prog_${cfg}.h TYPE APPL CREATOR DrmB)
    target_include_directories(DreamB${cfg} PRIVATE ${GEN} ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(DreamB${cfg} PRIVATE PROG_H="prog_${cfg}.h")
    target_compile_options(DreamB${cfg} PRIVATE -O2 -Wall -Wno-multichar -Wno-pointer-sign --param=min-pagesize=0)
    set_target_properties(DreamB${cfg} PROPERTIES LINK_FLAGS "-Wl,--mac-strip-macsbug -Wl,--mac-single")
endforeach()

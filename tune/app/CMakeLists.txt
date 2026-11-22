# Tune: the Mac composes and plays folk tunes (Retro68).
#   cmake -S tune/app -B tune/build/app -G Ninja -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   cmake --build tune/build/app            -> tune/build/app/Tune.bin
# The model (tune/model/tune_model.bin, from tune/ref/export.py) goes in as resource 'TMDL' 128.
cmake_minimum_required(VERSION 3.13)
project(Tune C ASM)
set(GEN ${CMAKE_BINARY_DIR})
add_custom_command(OUTPUT ${GEN}/kernels68k.s
    COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/gen_kernels.py ${GEN}/kernels68k.s
    DEPENDS gen_kernels.py)
set_source_files_properties(${GEN}/kernels68k.s PROPERTIES GENERATED TRUE LANGUAGE ASM)
set_source_files_properties(mix68k.s PROPERTIES LANGUAGE ASM)
if(NOT TUNE_MODEL)
    set(TUNE_MODEL ${CMAKE_CURRENT_SOURCE_DIR}/../model/tune_model.bin)
endif()
configure_file(${TUNE_MODEL} ${GEN}/model.bin COPYONLY)
set_source_files_properties(tune.r PROPERTIES OBJECT_DEPENDS ${GEN}/model.bin)
add_application(Tune
    main.c engine.c grammar.c tables.c staff.c glyphs.c player.c sndtables.c abc.c mix68k.s
    ${GEN}/kernels68k.s tune.r
    TYPE APPL
    CREATOR Tune
)
target_compile_options(Tune PRIVATE -O2 -ffunction-sections -Wall -Wno-pointer-sign -Wno-multichar
    --param=min-pagesize=0)
# one CODE segment: the sound interrupt tasks must never trigger a segment load
set_target_properties(Tune PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug -Wl,--mac-single")

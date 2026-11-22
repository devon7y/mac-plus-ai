# Scribble Calc: a handwriting calculator for the Mac Plus (Retro68).
#   cmake -S . -B ../build/app -G Ninja -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   cmake --build ../build/app            -> ../build/app/ScribbleCalc.bin
# The classifier's 68000 code and tables are generated from the trained integer model
# (calc/model/scribble_model.npz) by calc/ref/gen_net.py; the answer handwriting by gen_hand.py.
cmake_minimum_required(VERSION 3.13)
project(ScribbleCalc C ASM)

find_program(PYTHON3 NAMES python3 PATHS /opt/anaconda3/bin NO_DEFAULT_PATH)
find_program(PYTHON3 NAMES python3)
set(REF ${CMAKE_CURRENT_SOURCE_DIR}/../ref)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
set(MODEL ${CMAKE_CURRENT_SOURCE_DIR}/../model/scribble_model.npz)
add_custom_command(
    OUTPUT ${GEN}/net68k.s ${GEN}/net_gen.c ${GEN}/net_gen.h
    COMMAND ${PYTHON3} -I ${REF}/gen_net.py ${MODEL} ${GEN}
    DEPENDS ${MODEL} ${REF}/gen_net.py ${REF}/ref.py)
add_custom_command(
    OUTPUT ${GEN}/hand_gen.c ${GEN}/hand_gen.h
    COMMAND ${PYTHON3} -I ${REF}/gen_hand.py ${GEN}
    DEPENDS ${REF}/gen_hand.py ${REF}/synth.py)
set_source_files_properties(${GEN}/net68k.s PROPERTIES GENERATED TRUE LANGUAGE ASM)
set_source_files_properties(recog.c line.c main.c PROPERTIES OBJECT_DEPENDS ${GEN}/net_gen.h)
set_source_files_properties(draw.c PROPERTIES OBJECT_DEPENDS ${GEN}/hand_gen.h)
include_directories(${GEN} ${CMAKE_CURRENT_SOURCE_DIR})

add_application(ScribbleCalc
    main.c draw.c line.c recog.c dec.c calc.r
    ${GEN}/net_gen.c ${GEN}/net68k.s ${GEN}/hand_gen.c
    TYPE APPL
    CREATOR SCal
)
target_compile_options(ScribbleCalc PRIVATE -Os -ffunction-sections -Wall -Wno-pointer-sign -Wno-multichar --param=min-pagesize=0)
set_target_properties(ScribbleCalc PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug")

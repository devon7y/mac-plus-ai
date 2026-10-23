# Multi-segment test: three generated segments (about 28, 28 and 70 KB of code; the last
# one also holds a 40 KB const table, which ends up in DATA 0) plus Main.
cmake_minimum_required(VERSION 3.13)
project(SegTest C)

# -DSEG_SCALE=4 multiplies the generated code (about 0.5 MB) to probe size limits.
set(SEG_SCALE 1 CACHE STRING "multiplier for the generated code size")
# -DSEG_TABLE=code puts the 40 KB table into seg_c's CODE resource instead of DATA 0.
set(SEG_TABLE data CACHE STRING "where the const table goes: data or code")
set(GEN ${CMAKE_BINARY_DIR}/gen)
execute_process(COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/gen.py ${GEN} ${SEG_SCALE} ${SEG_TABLE}
                COMMAND_ERROR_IS_FATAL ANY)

add_application(SegTest
    main.c ${GEN}/seg_a.c ${GEN}/seg_b.c ${GEN}/seg_c.c
    TYPE APPL CREATOR SEGt)
target_include_directories(SegTest PRIVATE ${GEN})
target_compile_options(SegTest PRIVATE -Os -ffunction-sections -Wall -Wno-pointer-sign)
# --mac-segments puts each seg_*.c into its own CODE resource (see gen/segments.segmap).
set_target_properties(SegTest PROPERTIES
    LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug -Wl,--mac-segments -Wl,${GEN}/segments.segmap")

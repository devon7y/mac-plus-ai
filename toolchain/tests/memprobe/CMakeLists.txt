# Memory probe: asks for a 16 MB partition (memprobe.r) and reports what it gets.
cmake_minimum_required(VERSION 3.13)
project(MemProbe C)
add_application(MemProbe main.c memprobe.r TYPE APPL CREATOR MEMp)
target_compile_options(MemProbe PRIVATE -Os -ffunction-sections -Wall -Wno-pointer-sign --param=min-pagesize=0)
set_target_properties(MemProbe PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug")

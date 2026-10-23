# Test app for toolchain/lib (see main.c and check_lib.lua).
cmake_minimum_required(VERSION 3.13)
project(PlusLibTest C)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_BINARY_DIR}/pluslib)
add_application(LibTest main.c TYPE APPL CREATOR PLib)
target_link_libraries(LibTest PRIVATE plus)
target_compile_options(LibTest PRIVATE -Os -ffunction-sections -Wall -Wno-pointer-sign --param=min-pagesize=0)
set_target_properties(LibTest PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug")

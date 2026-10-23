# toolchain/lib as a static library. In a project's CMakeLists.txt:
#   add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../toolchain/lib ${CMAKE_BINARY_DIR}/pluslib)
#   target_link_libraries(MyGame PRIVATE plus)
cmake_minimum_required(VERSION 3.13)
add_library(plus STATIC ${CMAKE_CURRENT_LIST_DIR}/plus.c)
target_include_directories(plus PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(plus PRIVATE -O2 -Wall -Wno-pointer-sign --param=min-pagesize=0)

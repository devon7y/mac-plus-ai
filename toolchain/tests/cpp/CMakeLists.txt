# C++ smoke test. CppTest uses exceptions; CppTestNoEx is the same minus the exception
# check, built with -fno-exceptions -fno-rtti (to compare sizes).
cmake_minimum_required(VERSION 3.13)
project(CppTest CXX)

add_application(CppTest main.cc TYPE APPL CREATOR CPPt)
target_compile_features(CppTest PRIVATE cxx_std_17)
target_compile_options(CppTest PRIVATE -Os -ffunction-sections -Wall)
set_target_properties(CppTest PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug")

add_application(CppTestNoEx main.cc TYPE APPL CREATOR CPPn)
target_compile_features(CppTestNoEx PRIVATE cxx_std_17)
target_compile_definitions(CppTestNoEx PRIVATE NO_EXCEPTIONS)
target_compile_options(CppTestNoEx PRIVATE -Os -ffunction-sections -Wall -fno-exceptions -fno-rtti)
set_target_properties(CppTestNoEx PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug")

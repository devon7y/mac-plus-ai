# Cycle calibration test (see main.c): measures the emulator's effective 68000 clock.
cmake_minimum_required(VERSION 3.13)
project(CycleCal C)
add_application(CycleCal main.c TYPE APPL CREATOR CYCl)
target_compile_options(CycleCal PRIVATE -O2 -Wall -Wno-pointer-sign --param=min-pagesize=0)

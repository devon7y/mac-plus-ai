# Ask AI for the Mac Plus / SE (Retro68). Build (askai/build.py does all of this):
#   cmake -S askai/app -B askai/build/app -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   cmake --build askai/build/app        -> AskAI.bin (MacBinary; the app is called "Ask AI")
cmake_minimum_required(VERSION 3.13)
project(AskAI C)

add_application(AskAI
    main.c link.c serial.c askai.r
    TYPE APPL
    CREATOR AskA
)
target_compile_options(AskAI PRIVATE -Os -ffunction-sections -Wall -Wno-pointer-sign
    -Wno-discarded-qualifiers --param=min-pagesize=0)
set_target_properties(AskAI PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug -Wl,--mac-single")

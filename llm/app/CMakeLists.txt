# Pixel, the Mac Plus / SE chatbot (Retro68). See llm/app/README in llm/HANDOFF.md.
#   cmake -S . -B ../build/app/mac -G Ninja -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   cmake --build ../build/app/mac
cmake_minimum_required(VERSION 3.13)
project(Pixel C ASM)

add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/kernels68k.s
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/gen_kernels.py ${CMAKE_CURRENT_SOURCE_DIR}/kernels68k.s
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen_kernels.py)
add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/tables.c
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/gen_tables.py ${CMAKE_CURRENT_SOURCE_DIR}/tables.c
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen_tables.py ${CMAKE_CURRENT_SOURCE_DIR}/../ref/model.py)

add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/kernels68k_w4.s
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/gen_kernels.py ${CMAKE_CURRENT_SOURCE_DIR}/kernels68k_w4.s --no-b5
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen_kernels.py)

# Pixel: the Plus floppy build (W4 only: every KB saved on the boot floppy is load time).
# PixelSE: also the compact B5 kernels (System 7 SE, about 2 MB). PixelTest: everything plus the
# kernel self-test and profiling. All from the same sources. Only PixelSE has the on-screen
# keyboard (PX_KB, kb.r), opened when loading finishes.
foreach(app Pixel PixelSE PixelTest)
    if(app STREQUAL "PixelTest")
        set(extra kernels68k.s kernels_c_mac.c)
    elseif(app STREQUAL "PixelSE")
        set(extra kernels68k.s kb.r)            # the on-screen keyboard (Edit menu)
    else()
        set(extra kernels68k_w4.s)
    endif()
    add_application(${app}
        main.c engine.c tables.c pixel.r ${extra}
        TYPE APPL
        CREATOR Pxl1
    )
    target_compile_options(${app} PRIVATE -Os -ffunction-sections -Wall -Wno-pointer-sign -Wno-discarded-qualifiers)
    set_target_properties(${app} PROPERTIES LINK_FLAGS "-Wl,-gc-sections -Wl,--mac-strip-macsbug -Wl,--mac-single")
endforeach()
target_compile_definitions(Pixel PRIVATE PX_NO_B5)
target_compile_definitions(PixelSE PRIVATE PX_KB)          # the lab's SE has no keyboard
target_compile_definitions(PixelTest PRIVATE PIXEL_SELFTEST PX_PROFILE)

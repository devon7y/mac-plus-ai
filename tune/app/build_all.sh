#!/bin/zsh
# Build Tune and its floppies:  tune/app/build_all.sh
#   -> tune/build/app/Tune.bin, tune/build/Tune_icon.bin (with the Finder icon),
#      tune/build/tune_plus.img (800K Plus boot floppy), tune/build/tune_se.img (1.44 MB SE floppy)
set -e
A=${0:A:h}
R=${A:h:h}
cd $R
python3 -I tune/app/gen_consts.py
python3 -I tune/app/gen_glyphs.py
cmake -S tune/app -B tune/build/app -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake > /dev/null
touch tune/app/tune.r
cmake --build tune/build/app
python3 -I toolchain/icons/add_icon.py tune/build/app/Tune.bin --icon art/tune.txt --out tune/build/Tune_icon.bin
python3 -I tune/app/build_disk.py --app tune/build/Tune_icon.bin --out tune/build/tune_plus.img
python3 -I tune/app/make_se_floppy.py --app tune/build/app/Tune.bin --out tune/build/tune_se.img

#!/bin/zsh
# Build every toolchain test app and its boot disk into toolchain/build/.
#   toolchain/build_all.sh
# Apps: HelloPlus (template/), CppTest + CppTestNoEx (tests/cpp), SegTest (tests/segments),
#       MemProbe (tests/memprobe), LibTest (lib/test), CycleCal (tests/cyclecal).
# Plus disks: hello_narrow.img (Narrow Screen 32/32), hello_512.img (no Narrow Screen),
#        cpp_disk.img, cppnoex_disk.img, seg_disk.img, libtest_narrow.img (all with Narrow Screen).
# SE / System 7.0.1 hard disks (if emu/disks/system-7.0.1/se7_base.hd exists):
#        hello_se7.hd, cpp_se7.hd, memprobe_se7.hd, seg_se7.hd, libtest_se7.hd (apps in Startup Items).
set -e
T=${0:A:h}
B=$T/build
RETRO68=${RETRO68:-$HOME/Retro68-build/toolchain}
TC=$RETRO68/m68k-apple-macos/cmake/retro68.toolchain.cmake
[[ -f $TC ]] || { echo "Retro68 toolchain file not found: $TC"; exit 1; }
mkdir -p $B
for p in template:template tests/cpp:cpp tests/segments:segments tests/memprobe:memprobe lib/test:libtest tests/cyclecal:cyclecal; do
  src=$T/${p%%:*}; out=$B/${p##*:}
  cmake -S $src -B $out -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC > /dev/null
  cmake --build $out
done
mk() { python3 -I $T/template/build_disk.py --app $1 --out $B/$2 "${@:3}" | tail -2; }
mk $B/template/HelloPlus.bin hello_narrow.img
mk $B/template/HelloPlus.bin hello_512.img --no-narrow
mk $B/cpp/CppTest.bin cpp_disk.img --label "Cpp Test"
mk $B/cpp/CppTestNoEx.bin cppnoex_disk.img --label "Cpp NoEx"
mk $B/segments/SegTest.bin seg_disk.img --label "Seg Test"
mk $B/libtest/LibTest.bin libtest_narrow.img --label "Lib Test"
mk $B/cyclecal/CycleCal.bin cyclecal.img --keep-se --label "CycleCal"   # boots Plus and SE
if [[ -f $T/../emu/disks/system-7.0.1/se7_base.hd ]]; then
  se() { python3 -I $T/se7/build_se_disk.py --app $1 --out $B/$2 | tail -1; }
  se $B/template/HelloPlus.bin hello_se7.hd
  se $B/cpp/CppTest.bin cpp_se7.hd
  se $B/memprobe/MemProbe.bin memprobe_se7.hd
  se $B/segments/SegTest.bin seg_se7.hd
  se $B/libtest/LibTest.bin libtest_se7.hd
fi

#!/bin/zsh
# Build everything for Pixel and check the host engine against the Python reference.
#   llm/app/build_all.sh [model.pxm]      (default: a random-weight test model)
# Outputs (llm/build/app/, gitignored):
#   mac/Pixel.bin, PixelSE.bin, PixelTest.bin  Plus build (W4), SE build (W4 + B5), test build
#   release/pixel_boot.img, pixel_disk2.img     the Plus floppy set (800K each)
#   release/pixel_se7.hd, release/se/           SE hard disk (System 7.0.1, MacinTalk) and the SE folder files
#   release/pixel_se_144_1.img, _2.img          the SE's 1.44 MB pair (raw model; MacinTalk on disk 1)
#   release/pixelse_app_144.img                 PixelSE and MacinTalk alone (to update the SE's hard disk)
#   test/...                                    the same with PixelTest and a "Pixel Test" script
set -e
A=${0:A:h}
L=$A/..
B=$L/build/app
TC=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
mkdir -p $B
python3 $A/gen_kernels.py $A/kernels68k.s
python3 $A/gen_tables.py $A/tables.c
python3 $A/test_host.py 70                         # builds llm/build/app/pxhost, differential test
[[ -d $B/mac ]] || cmake -S $A -B $B/mac -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC > /dev/null
cmake --build $B/mac
MODEL=${1:-$B/rand2.pxm}
if [[ ! -f $MODEL ]]; then
  python3 $L/ref/make_test_tokenizer.py
  python3 -c "
import sys; sys.path.insert(0, '$L/ref')
import export
tok = export.load_tokenizer('$L/build/ref/test_tokenizer.json')
pm = export.export(export.random_float_weights(2, 0.08)); pm['merges'] = export.tokenizer_merges(tok)
export.write_model(pm, '$MODEL')"
fi
python3 $A/make_disks.py --model $MODEL --app $B/mac/Pixel.bin --se-app $B/mac/PixelSE.bin --out $B/release
python3 $A/make_se_floppy.py --model $MODEL --app $B/mac/PixelSE.bin --out $B/release --raw
python3 $A/make_se_floppy.py --model $MODEL --app $B/mac/PixelSE.bin --out $B/release --layout app
python3 $A/make_disks.py --model $MODEL --app $B/mac/PixelTest.bin --out $B/test --slack2 1024 \
  --test "12345 205 16 12 0" "hi pixel" "what is your name?" "reset" "do you like floppy disks?"

#!/bin/zsh
# Build Dream: both apps, the host bit-exactness check and the boot floppies.
#   dream/app/build_all.sh [model64.drm] [model32.drm]
# Defaults: dream/data/models/dream64.drm and dream32.drm if present, else random test models.
# Outputs (gitignored, dream/build/):
#   app/Dream.bin, app/DreamSm.bin       4 MB (compiled layers) and 1 MB (lookup tables) apps
#   release/dream.img                    800K floppy, 4 MB Plus; also boots an SE (PTCH 630 kept)
#   release/dream_1mb.img                800K floppy, 1 MB Plus, 32x32 model
set -e
A=${0:A:h}
D=$A/..
B=$D/build
TC=$HOME/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
mkdir -p $B/release $B/models
M64=${1:-$D/data/models/dream64.drm}
M32=${2:-$D/data/models/dream32.drm}
if [[ ! -f $M64 ]]; then M64=$B/models/rand_A64L2.drm; python3 $D/engine/model_file.py random A64L2 $M64 --subjects 250; fi
if [[ ! -f $M32 ]]; then M32=$B/models/rand_A32S.drm; python3 $D/engine/model_file.py random A32S $M32 --subjects 250; fi
[[ -d $B/app ]] || cmake -S $A -B $B/app -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC > /dev/null
cmake --build $B/app
python3 $A/test_host.py $M64 0 1
python3 $A/test_host.py $M32 0 1
python3 $A/make_disk.py --model $M64 --app $B/app/Dream.bin --out $B/release/dream.img --se
python3 $A/make_disk.py --model $M32 --app $B/app/DreamSm.bin --out $B/release/dream_1mb.img
echo "built: $B/release/dream.img (4 MB, model $M64), $B/release/dream_1mb.img (1 MB, model $M32)"

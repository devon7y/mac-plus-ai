#!/bin/zsh
# M6: build the Doodle boot floppy (800K): trimmed System 6.0, Narrow Screen, Doodle, "Doodle Model".
#   make_floppy.sh [model.dmdl] [out.img]
# Default model: paint/app/data/Doodle.dmdl if present, else a random H=384 ternary test model.
set -e
APP=${0:A:h}; ROOT=$APP/../..
PY=/opt/anaconda3/bin/python3
MODEL=${1:-$APP/data/Doodle.dmdl}
OUT=${2:-$ROOT/paint/build/Doodle.img}
W=$ROOT/paint/build/floppy; mkdir -p $W
if [[ ! -f $MODEL ]]; then
  echo "no trained model at $MODEL: using a random test model"
  MODEL=$W/random384.dmdl
  $PY $ROOT/paint/bench/export.py random $MODEL --H 384
fi
$PY $ROOT/paint/bench/model_file.py macbinary $MODEL $W/DoodleModel.bin
cmake --build $APP/build >/dev/null
python3 -I $ROOT/toolchain/icons/add_icon.py $APP/build/Doodle.bin --preset doodle    # Finder icon + bundle
python3 -I $APP/build_disk.py --app $APP/build/Doodle.bin --out $OUT --label Doodle --se --extra $W/DoodleModel.bin

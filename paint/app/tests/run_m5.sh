#!/bin/zsh
# M5 end-to-end: model file -> disk (app + "Doodle Model"), Finish it in MAME (1 MB Plus) via the
# mailbox and via the UI, then compare every generated move with paint/bench/ref.py.
#   MODEL=<file.dmdl> (default: a random H=384 ternary model) OUT=<dir> run_m5.sh
set -e
T=${0:A:h}; APP=$T/..; ROOT=$APP/../..
OUT=${OUT:-$ROOT/paint/build/m5}
rm -rf $OUT; mkdir -p $OUT
PY=/opt/anaconda3/bin/python3
if [[ -z "$MODEL" ]]; then
  MODEL=$OUT/model.dmdl
  $PY $ROOT/paint/bench/export.py random $MODEL --H ${H:-384} --wh ${WH:-tern} --wo ${WO:-tern}
fi
$PY $ROOT/paint/bench/model_file.py macbinary $MODEL $OUT/DoodleModel.bin
cmake --build $APP/build >/dev/null
python3 -I $APP/build_disk.py --out $OUT/m5_disk.img --extra $OUT/DoodleModel.bin | tail -2
PAINT_TESTS=$T TEST_OUT=$OUT RUN_DIR=$OUT/run $ROOT/emu/run_test.sh $OUT/m5_disk.img $T/test_finish.lua ${SECS:-1500} | tee $OUT/log.txt | grep -v "CHECK.* ok"
$PY $T/check_m5.py $MODEL $OUT cat

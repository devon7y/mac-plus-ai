#!/bin/zsh
# Doodle on the emulated Macintosh SE (4 MB) under System 7.0.1 (toolchain/se7 testbed): the app in
# Startup Items (launched by the Finder), "Doodle Model" at the top level of the disk, then the
# Finish it test (mailbox + UI) and the move-by-move comparison with paint/bench/ref.py.
#   MODEL=<file.dmdl> OUT=<dir> run_se7.sh
set -e
T=${0:A:h}; APP=$T/..; ROOT=$APP/../..
PY=/opt/anaconda3/bin/python3
OUT=${OUT:-$ROOT/paint/build/se7}
rm -rf $OUT; mkdir -p $OUT
MODEL=${MODEL:-$ROOT/paint/build/floppy/random384.dmdl}
[[ -f $MODEL ]] || $PY $ROOT/paint/bench/export.py random $MODEL --H 384
$PY $ROOT/paint/bench/model_file.py macbinary $MODEL $OUT/DoodleModel.bin
cmake --build $APP/build >/dev/null
HD=${SE7_HD:-$ROOT/paint/build/doodle_se7.hd}
python3 -I $ROOT/toolchain/se7/build_se_disk.py --app $APP/build/Doodle.bin --out $HD \
    --extra $OUT/DoodleModel.bin | tail -2
MACHINE=macsefd RAM=4M HARDDISK=$HD BLOCK_WAIT=180 PAINT_TESTS=$T TEST_OUT=$OUT \
    RUN_DIR=$OUT/run $ROOT/emu/run_test.sh none $T/${SE7_TEST:-test_finish.lua} ${SECS:-1500} | tee $OUT/log.txt | grep -v "CHECK.* ok"
[[ ${SE7_TEST:-test_finish.lua} == test_finish.lua ]] && $PY $T/check_m5.py $MODEL $OUT cat

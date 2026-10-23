#!/bin/zsh
# M2 end-to-end: build Doodle, make a disk with a PNG-converted MacPaint fixture, run the tool
# test in MAME (1 MB Plus), then check every save/open round trip on the MacBook.
set -e
T=${0:A:h}; APP=$T/..; ROOT=$APP/../..
OUT=${OUT:-$ROOT/paint/build/m2}
rm -rf $OUT; mkdir -p $OUT
cmake --build $APP/build >/dev/null
/opt/anaconda3/bin/python3 $T/check_m2.py fixture $OUT
python3 -I $APP/build_disk.py --out $OUT/m2_disk.img --extra $OUT/Test7.bin >/dev/null
PAINT_TESTS=$T TEST_OUT=$OUT RUN_DIR=$OUT/run $ROOT/emu/run_test.sh $OUT/m2_disk.img $T/test_paint.lua ${SECS:-400} | tee $OUT/log.txt | grep -v "CHECK.* ok"
/opt/anaconda3/bin/python3 $T/check_m2.py verify $OUT $OUT/run/disk1.img

#!/bin/zsh
# Ship a trained model: export the checkpoint, draw the sample sheet with the Mac's arithmetic,
# build the floppy, run Finish it in MAME on the Plus (1 MB) and the SE (4 MB) from the floppy,
# and on the SE under System 7.0.1 from a hard disk, comparing every generated move with ref.py.
#   run_final.sh <checkpoint.pt | model.dmdl>
# Rehearsals: MODEL_OUT=, FLOPPY=, SHEET= and RUNS= (run-folder prefix) keep the shipped files untouched.
set -e
T=${0:A:h}; APP=$T/..; ROOT=$APP/../..
PY=/opt/anaconda3/bin/python3
SRC=${1:?checkpoint or model file}
MODEL=${MODEL_OUT:-$APP/data/Doodle.dmdl}
FLOPPY=${FLOPPY:-$ROOT/paint/build/Doodle.img}
SHEET=${SHEET:-$ROOT/paint/build/final_samples.png}
RUNS=${RUNS:-$ROOT/paint/build/final_}
if [[ $SRC == *.dmdl ]]; then [[ $SRC -ef $MODEL ]] || cp $SRC $MODEL; else $PY $ROOT/paint/bench/export.py ckpt $SRC $MODEL; fi
$PY $ROOT/paint/bench/sample_sheet.py $MODEL $SHEET
$APP/make_floppy.sh $MODEL $FLOPPY | tail -2
for M in plus se; do
  O=$RUNS$M; rm -rf $O; mkdir -p $O
  if [[ $M == se ]]; then export MACHINE=macsefd RAM=4M BLOCK_WAIT=150; else unset MACHINE RAM BLOCK_WAIT; fi
  PAINT_TESTS=$T TEST_OUT=$O RUN_DIR=$O/run $ROOT/emu/run_test.sh $FLOPPY $T/test_finish.lua 1500 \
    > $O/log.txt 2>&1 &
done
wait
unset MACHINE RAM BLOCK_WAIT
MODEL=$MODEL OUT=${RUNS}se7 SE7_HD=${RUNS}se7.hd $T/run_se7.sh > /dev/null 2>&1 || true
for M in plus se se7; do
  echo "== $M"; grep -E "RESULT|FAIL" $RUNS$M/log.txt
  $PY $T/check_m5.py $MODEL $RUNS$M cat
done
echo "sample sheet: $SHEET; floppy: $FLOPPY"

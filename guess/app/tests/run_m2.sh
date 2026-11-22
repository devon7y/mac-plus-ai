#!/bin/zsh
# M2 check: build the Plus floppy for a model, boot it in MAME (1 MB Plus), force round 1's word,
# draw it with the mouse from a Lua script and check that guesses appear.
#   run_m2.sh [model.gmdl] [drawing=house] [extra env for run_test.sh, e.g. SLOW=1.5]
set -e
T=${0:A:h}; G=$T/../..; ROOT=$G/..
MODEL=${1:-$G/app/data/Guess.gmdl}
DRAWING=${2:-house}
cmake --build $G/build/app >/dev/null
python3 -I $G/app/make_disks.py --model $MODEL --only plus --out $G/build/m2 >/dev/null
TARGET=$(python3 -c "
import sys; sys.path.insert(0, '$G/engine')
import model_file
n = [x.split('|', 1)[1] for x in model_file.read('$MODEL')['names']]
print(n.index('$DRAWING') if '$DRAWING' in n else 0)")
echo "target $DRAWING = category $TARGET"
GUESS_TESTS=$T TARGET=$TARGET DRAWING=$DRAWING RUN_DIR=${RUN_DIR:-$G/build/runs/m2-$(date +%H%M%S)} \
  $ROOT/emu/run_test.sh $G/build/m2/guess_plus.img $T/m2.lua 150

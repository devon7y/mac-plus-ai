#!/bin/zsh
# Orbit Duel M1: two MAME Mac Pluses joined through their modem ports (null modem + TCP socket),
# both running the serial cable game with the computer flying each Mac's ship, in real time.
# Passes if both see the game start, compare >= 10 state checksums with no mismatch, and print
# the same checksums for steps 30..240.
#   duel/tests/serial_link.sh [play seconds, default 60]
#   B_MACHINE=macsefd B_RAM=4M B_IMG=duel/build/orbitduel_se.img duel/tests/serial_link.sh   (Plus <-> SE)
D=${0:A:h:h}
R=${D:h}
PORT=${PORT:-24680}
PLAY=${1:-60}
IMG=$D/build/orbitduel.img
export LINK=4 PLAY_S=$PLAY
OUT=$R/emu/runs/duel-serial-$(date +%Y%m%d-%H%M%S)
mkdir -p $OUT
# A starts first: its bitbanger can't connect yet, so it listens; B then connects to it.
SIDE=A RUN_DIR=$OUT/A MAME_ARGS="-throttle -modem null_modem -bitb socket.127.0.0.1:$PORT" \
  $R/emu/run_test.sh $IMG $D/tests/link.lua $((PLAY + 160)) > $OUT/A.log 2>&1 &
PA=$!
sleep 3
SIDE=B RUN_DIR=$OUT/B MACHINE=${B_MACHINE:-macplus} RAM=${B_RAM:-1M} CHECK_AT=${B_CHECK_AT:-12} MAME_ARGS="-throttle -modem null_modem -bitb socket.127.0.0.1:$PORT" \
  $R/emu/run_test.sh ${B_IMG:-$IMG} $D/tests/link.lua $((PLAY + 200)) > $OUT/B.log 2>&1 &
PB=$!
wait $PA $PB
grep -h "^\[" $OUT/A.log $OUT/B.log
SA=$(grep -h "SUMS" $OUT/A.log | sed 's/.*SUMS //')
SB=$(grep -h "SUMS" $OUT/B.log | sed 's/.*SUMS //')
if [[ -n "$SA" && "$SA" == "$SB" ]] && grep -q "RESULT PASS" $OUT/A.log && grep -q "RESULT PASS" $OUT/B.log; then
  echo "LINK RESULT PASS (identical checksums on both Macs) -> $OUT"
else
  echo "LINK RESULT FAIL -> $OUT"
fi

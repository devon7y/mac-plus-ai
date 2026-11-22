#!/bin/zsh
# The whole Shuttle chain in MAME: Shuttle (test floppy) -> modem port -> null modem -> Hayes modem
# emulator (alias "mac", the Plus Modem's boot banner) -> shuttle_server.py serving a fresh test folder.
#   shuttle/tests/run_chain.sh [emulated seconds, default 900]
#   TARGET=se7 shuttle/tests/run_chain.sh      the same on the SE / System 7.0.1 testbed (SCSI hard disk)
# Builds the app and test floppy, makes the server's folder in the run folder (root/), starts the server and
# the modem emulator, runs one MAME instance, stops the helpers, then verify.py compares the files on both
# sides. Logs and screenshots: the printed RUN_DIR (mame.log, server.log, modem.log, shots/).
# Note: on the Plus floppy, MAME's saving of the written 800K image drops a few sectors (zeros in the
# image file; see narrowscreen/README.md), although the Mac itself reads the file back correctly (Shuttle
# checks every download). verify.py then fails on those sectors; the SE run (SCSI disk) is byte-exact.
# Knobs: SERVER_PORT (16801), MODEM_PORT (23643), BAUD (null-modem speed index, 21 = 7200 = the app's MAME
# test speed), MAME_EXTRA (e.g. -throttle), SCRIPT (transfer.lua).
A=${0:A:h:h}
R=${A:h}
SERVER_PORT=${SERVER_PORT:-16801}
MODEM_PORT=${MODEM_PORT:-23643}
export RUN_DIR=$R/emu/runs/shuttle-$(date +%Y%m%d-%H%M%S)-$$
mkdir -p $RUN_DIR
if [[ "${TARGET:-plus}" == se7 ]]; then
  python3 -I $A/build.py --test-only --test-se7 || exit 1
else
  python3 -I $A/build.py --test-only || exit 1
fi
python3 -I $A/tests/make_root.py $RUN_DIR/root || exit 1
python3 -u $A/server/shuttle_server.py --root $RUN_DIR/root --cache $RUN_DIR/cache --port $SERVER_PORT \
  --host 127.0.0.1 --name TestBook > $RUN_DIR/server.log 2>&1 &
SERVER=$!
python3 -u $A/tests/hayes_emu.py --listen 127.0.0.1:$MODEM_PORT --alias mac=127.0.0.1:$SERVER_PORT ${BANNER:---banner} \
  > $RUN_DIR/modem.log 2>&1 &
MODEM=$!
sleep 1
export SHUTTLE_TESTS=$A/tests
if [[ "${TARGET:-plus}" == se7 ]]; then
  LIMIT=${LIMIT:-1050} MACHINE=macsefd RAM=4M HARDDISK=$A/build/test/shuttle_se7.hd \
  MAME_ARGS="-modem null_modem -bitb socket.127.0.0.1:$MODEM_PORT ${MAME_EXTRA:-}" \
    $R/emu/run_test.sh none $A/tests/${SCRIPT:-transfer.lua} ${1:-1000} | tee $RUN_DIR/mame.log
else
  MAME_ARGS="-modem null_modem -bitb socket.127.0.0.1:$MODEM_PORT ${MAME_EXTRA:-}" \
    $R/emu/run_test.sh $A/build/test/shuttle_test.img $A/tests/${SCRIPT:-transfer.lua} ${1:-900} | tee $RUN_DIR/mame.log
fi
kill $SERVER $MODEM 2>/dev/null
echo "--- server.log"; tail -n 25 $RUN_DIR/server.log
echo "--- modem.log"; tail -n 8 $RUN_DIR/modem.log
[[ "${SCRIPT:-transfer.lua}" == transfer.lua ]] && python3 -I $A/tests/verify.py $RUN_DIR

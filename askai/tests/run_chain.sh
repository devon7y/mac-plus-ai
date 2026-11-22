#!/bin/zsh
# The whole Ask AI chain in MAME: app (test floppy) -> modem port -> null modem -> Hayes modem emulator
# -> relay (mock provider, no network, no keys) -> streamed replies on the Mac's screen.
#   askai/tests/run_chain.sh [emulated seconds, default 200]       (SCRIPT=m2.lua for the M2 check)
# Builds the test floppy (prefs dial 127.0.0.1:$RELAY_PORT, PIN 2468), starts the relay and the modem
# emulator in the background, runs one MAME instance (real-time, so the relay's timing is realistic),
# then stops the helpers. Logs and screenshots: the printed RUN_DIR (relay.log, modem.log, shots/).
# Knobs: RELAY_PORT (16800), MODEM_PORT (23642), BAUD (null-modem speed index, 21 = 7200 = the app's MAME test speed), MAME_EXTRA, MODEM_EXTRA (--log-data).
A=${0:A:h:h}
R=${A:h}
RELAY_PORT=${RELAY_PORT:-16800}
MODEM_PORT=${MODEM_PORT:-23642}
export RUN_DIR=$R/emu/runs/askai-$(date +%Y%m%d-%H%M%S)-$$
mkdir -p $RUN_DIR
python3 -I $A/build.py --no-release --test-dial 127.0.0.1:$RELAY_PORT --test-pin 2468 || exit 1
# a clean environment: the relay never sees real API keys
env -i PATH=/usr/bin:/bin:/opt/homebrew/bin HOME=$RUN_DIR python3 -u $A/relay/relay.py --mock-only \
  --host 127.0.0.1 --port $RELAY_PORT --pin 2468 > $RUN_DIR/relay.log 2>&1 &
RELAY=$!
env -i PATH=/usr/bin:/bin:/opt/homebrew/bin HOME=$RUN_DIR python3 -u $A/modem/hayes_emu.py \
  --listen 127.0.0.1:$MODEM_PORT ${MODEM_EXTRA:-} > $RUN_DIR/modem.log 2>&1 &
MODEM=$!
sleep 1
export ASKAI_TESTS=$A/tests
MAME_ARGS="-throttle -modem null_modem -bitb socket.127.0.0.1:$MODEM_PORT ${MAME_EXTRA:-}" \
  $R/emu/run_test.sh $A/build/test/askai_test.img $A/tests/${SCRIPT:-chat.lua} ${1:-200}
kill $RELAY $MODEM 2>/dev/null
echo "--- relay.log"; tail -n 20 $RUN_DIR/relay.log
echo "--- modem.log"; tail -n 20 $RUN_DIR/modem.log

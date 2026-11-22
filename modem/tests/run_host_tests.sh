#!/bin/bash
# Host tests for the Plus Modem firmware's portable core (about a minute, no hardware, only 127.0.0.1):
#   1. host_tests.cpp: Hayes lines, +++, the AI relay (mock model and recorded provider streams),
#      JSON, HTTP, SSE, settings helpers; built with AddressSanitizer and UBSan;
#   2. test_textconv.py: the Markdown -> Mac Roman converter against askai/relay/textconv.py;
#   3. test_e2e.py: the core as a simulator on TCP sockets, driven by the real tools (relaytest.py,
#      console commands) against a fake model API and askai's own relay.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
FW="$HERE/../firmware/plusmodem"
OUT="$HERE/../build/tests"
mkdir -p "$OUT"
SRC="$FW/config.cpp $FW/json.cpp $FW/http.cpp $FW/apis.cpp $FW/relay.cpp $FW/modem.cpp $FW/textconv.cpp"
clang++ -std=c++11 -Wall -Wextra -Wno-unused-parameter -g -O1 -fsanitize=address,undefined -fno-rtti \
  -o "$OUT/host_tests" "$HERE/host_tests.cpp" $SRC
"$OUT/host_tests"
python3 -I "$HERE/test_textconv.py" --cases 1500
clang++ -std=c++11 -Wall -Wextra -Wno-unused-parameter -g -O1 -fsanitize=address,undefined \
  -o "$OUT/sim" "$HERE/sim.cpp" $SRC
PY="$HERE/../build/venv/bin/python"
[ -x "$PY" ] || PY=python3
"$PY" -B "$HERE/test_e2e.py"

#!/bin/bash
# Install the pinned ESP32 toolchain for Plus Modem into modem/build/ (about 0.8 GB, no sudo).
#
#   modem/tools/toolchain.sh
#
# Arduino core for ESP32 2.0.17 (the last 2.x release, ESP-IDF 4.4) in arduino-esp32's documented
# "hardware folder" layout, with only the ESP32 (xtensa) compiler and esptool. `arduino-cli core
# install` would also fetch the RISC-V and ESP32-S2/S3 compilers (about 3.5 GB unpacked), which this
# board doesn't need. Archives are checked against the SHA-256 sums in Espressif's package index and
# downloaded to $TMPDIR, which the OS cleans up. arduino-cli itself comes from Homebrew.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"          # modem/
B="$HERE/build"
CORE_VER=2.0.17
CORE_URL=https://github.com/espressif/arduino-esp32/releases/download/2.0.17/esp32-2.0.17.zip
CORE_SHA=1f8658d4b18a8001ce782142ad08164af2991d70b83a147c3437a6ee30a9b225
GCC_URL=https://github.com/espressif/crosstool-NG/releases/download/esp-2021r2-patch5/xtensa-esp32-elf-gcc8_4_0-esp-2021r2-patch5-macos-arm64.tar.gz
GCC_SHA=b14189772d70a96813895fff7731d0f2fec0c825cfc02e002d6d91a0cc4b6b1d
if [ "$(uname -m)" != arm64 ]; then
  GCC_URL=https://github.com/espressif/crosstool-NG/releases/download/esp-2021r2-patch5/xtensa-esp32-elf-gcc8_4_0-esp-2021r2-patch5-macos.tar.gz
  GCC_SHA=19af109fda024a3a4c989f7ccaa104f9b1b74cfd6c9363e730bb8cb9b50d5dc4
fi
ESPTOOL_URL=https://github.com/espressif/arduino-esp32/releases/download/2.0.7/esptool-v4.5.1-macos.tar.gz
ESPTOOL_SHA=78b52acfd51541ceb97cee893b7d4d49b8ddc284602be8c73ea47e3d849e0956

PLAT="$B/arduino/hardware/espressif/esp32"         # arduino-cli "user" dir = $B/arduino
DL="${TMPDIR:-/tmp}/plusmodem-dl"
mkdir -p "$DL" "$B/arduino15"

fetch() {   # url sha -> path of the verified archive
  local f="$DL/$(basename "$1")"
  if [ ! -f "$f" ] || [ "$(shasum -a 256 "$f" | cut -d' ' -f1)" != "$2" ]; then
    echo "downloading $(basename "$1")" >&2
    curl -fL --retry 3 -o "$f.part" "$1"
    mv -f "$f.part" "$f"
  fi
  [ "$(shasum -a 256 "$f" | cut -d' ' -f1)" = "$2" ] || { echo "checksum mismatch: $f" >&2; exit 1; }
  echo "$f"
}

command -v arduino-cli >/dev/null || brew install arduino-cli

if [ ! -f "$PLAT/platform.txt" ]; then
  z="$(fetch "$CORE_URL" "$CORE_SHA")"
  [ ! -e "$PLAT" ] || { echo "$PLAT exists but is incomplete; move it aside and rerun" >&2; exit 1; }
  tmp="$B/core-unpack-$$"
  mkdir -p "$tmp" "$(dirname "$PLAT")"
  # skip the other chips' precompiled SDKs (esp32 only on this board)
  unzip -q "$z" -d "$tmp" -x '*/tools/sdk/esp32s2/*' '*/tools/sdk/esp32s3/*' '*/tools/sdk/esp32c3/*'
  mv "$tmp"/esp32-* "$PLAT"
fi
if [ ! -x "$PLAT/tools/xtensa-esp32-elf/bin/xtensa-esp32-elf-gcc" ]; then
  t="$(fetch "$GCC_URL" "$GCC_SHA")"
  tar -xzf "$t" -C "$PLAT/tools"                     # -> tools/xtensa-esp32-elf/
fi
if [ ! -x "$PLAT/tools/esptool/esptool" ]; then
  t="$(fetch "$ESPTOOL_URL" "$ESPTOOL_SHA")"
  mkdir -p "$PLAT/tools/esptool"
  tar -xzf "$t" -C "$PLAT/tools/esptool" --strip-components 1
fi

cat > "$B/arduino-cli.yaml" <<EOF
directories:
  data: $B/arduino15
  user: $B/arduino
  downloads: $DL
updater:
  enable_notification: false
EOF

# a venv for the host tools (pyserial)
if [ ! -x "$B/venv/bin/python" ]; then
  python3 -m venv "$B/venv"
  "$B/venv/bin/pip" install -q --disable-pip-version-check pyserial==3.5
fi
echo "toolchain ready: Arduino core for ESP32 $CORE_VER in $PLAT"

#!/bin/bash
# Build the Plus Modem firmware with the pinned Arduino core and flash it over USB.
#
#   modem/tools/flash.sh              build, then flash the board on /dev/cu.usbserial* or /dev/cu.SLAB_USBtoUART
#   modem/tools/flash.sh --build      build only (modem/build/firmware/plusmodem.ino.bin)
#   modem/tools/flash.sh --port /dev/cu.usbserial-0001
#
# The first run installs the toolchain (modem/tools/toolchain.sh, about 0.8 GB in modem/build/).
# Settings, Wi-Fi networks and API keys live in the ESP32's NVS partition and survive reflashing.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"           # modem/
B="$HERE/build"
PLAT="$B/arduino/hardware/espressif/esp32"
OUT="$B/firmware"
# ESP32 Dev Module, 4 MB flash in DIO mode (safe on clone boards), 1.9 MB app partitions
FQBN="espressif:esp32:esp32:PartitionScheme=min_spiffs,FlashMode=dio,FlashFreq=80,UploadSpeed=921600,DebugLevel=none"

build=1 upload=1 port=""
while [ $# -gt 0 ]; do
  case "$1" in
    --build) upload=0 ;;
    --port) port="$2"; shift ;;
    *) echo "usage: $0 [--build] [--port /dev/cu.xxx]" >&2; exit 2 ;;
  esac
  shift
done

[ -f "$PLAT/platform.txt" ] && [ -f "$B/arduino-cli.yaml" ] || "$HERE/tools/toolchain.sh"

PROPS=(--build-property "runtime.tools.xtensa-esp32-elf-gcc.path=$PLAT/tools/xtensa-esp32-elf"
       --build-property "runtime.tools.esptool_py.path=$PLAT/tools/esptool")

if [ $build = 1 ]; then
  mkdir -p "$OUT"
  arduino-cli --config-file "$B/arduino-cli.yaml" compile --fqbn "$FQBN" "${PROPS[@]}" \
    --build-path "$B/build-cache" --output-dir "$OUT" --warnings default "$HERE/firmware/plusmodem"
fi

if [ $upload = 1 ]; then
  if [ -z "$port" ]; then
    port="$(ls /dev/cu.usbserial* /dev/cu.SLAB_USBtoUART* 2>/dev/null | head -1 || true)"
  fi
  if [ -z "$port" ]; then
    echo "No ESP32 found on USB (/dev/cu.usbserial* or /dev/cu.SLAB_USBtoUART)." >&2
    echo "Check the cable carries data, then: ls /dev/cu.*" >&2
    exit 1
  fi
  echo "flashing $port"
  # esptool resets the board into its bootloader by itself (DTR/RTS)
  "$PLAT/tools/esptool/esptool" --chip esp32 --port "$port" --baud 921600 --before default_reset --after hard_reset \
    write_flash -z --flash_mode dio --flash_freq 80m --flash_size 4MB \
    0x1000 "$OUT/plusmodem.ino.bootloader.bin" 0x8000 "$OUT/plusmodem.ino.partitions.bin" \
    0xe000 "$PLAT/tools/partitions/boot_app0.bin" 0x10000 "$OUT/plusmodem.ino.bin"
fi

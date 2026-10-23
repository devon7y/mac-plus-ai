#!/bin/zsh
# Headless, scripted MAME run that is safe to run many times in parallel.
#   run_test.sh <disk image> <script.lua> [emulated seconds, default 120]
#   (disk image "none" = no floppy, e.g. to boot from HARDDISK)
# Each run gets its own folder (printed first as RUN_DIR=...) holding MAME settings, screenshots
# (shots/) and a private copy of the disk, so parallel runs never overwrite each other and the
# source disk is never modified (MAME writes changes back into disk images).
# Environment knobs:
#   RAM=1M|2M|4M      memory size (default 1M)
#   MACHINE=macplus   MAME machine (default macplus; e.g. macsefd for an SE FDHD)
#   FLOP2=<image>     second floppy (copied too)
#   HARDDISK=<image>  SCSI hard disk, ID 6 (copied too; MAME wants a .hd/.hdv/.chd extension)
#   RUN_DIR=<dir>     use this folder instead of a new one under emu/runs/
#   NO_COPY=1         use the disk images in place (they will be modified)
#   MAME_ARGS="..."   extra MAME options
#   SLOW=<factor>     do 1/factor of the 68000's work per frame, to approximate the real machine
#                     (MAME runs the Plus/SE CPU at the full 7.83 MHz; recommended SLOW=1.5 for the
#                     Plus, 1.3 for the SE: see toolchain/README.md, "Real-hardware speed")
# Lua scripts can read $EMU (this folder), $RUN_DIR, $DISK and $HDISK (the disks actually used).
E=${0:A:h}
export EMU=$E
export RUN_DIR=${RUN_DIR:-$E/runs/$(date +%Y%m%d-%H%M%S)-$$}
mkdir -p "$RUN_DIR/shots" "$RUN_DIR/cfg" "$RUN_DIR/nvram" "$RUN_DIR/diff"
echo "RUN_DIR=$RUN_DIR"

prep() {   # copy a disk image into the run folder unless NO_COPY=1
  if [[ -n "${NO_COPY:-}" ]]; then echo "$1"; return; fi
  local dst="$RUN_DIR/$2.${1:e}"
  cp "$1" "$dst" && chmod u+w "$dst" && echo "$dst"   # copies of read-only sources must stay writable
}
FLOP1_ARGS=()
export DISK=""
[[ "$1" != none ]] && { DISK=$(prep "$1" disk1); FLOP1_ARGS=(-flop1 "$DISK"); }
FLOP2_ARGS=()
[[ -n "${FLOP2:-}" ]] && FLOP2_ARGS=(-flop2 "$(prep "$FLOP2" disk2)")
HARD_ARGS=()
export HDISK=""
[[ -n "${HARDDISK:-}" ]] && { HDISK=$(prep "$HARDDISK" hard1); HARD_ARGS=(-hard "$HDISK"); }

SCRIPT=$2
if [[ -n "${SLOW:-}" ]]; then   # load scripts/slow.lua before the caller's script
  SCRIPT="$RUN_DIR/slow_wrapper.lua"
  printf 'dofile("%s")\ndofile("%s")\n' "${${E}//\"/\\\"}/scripts/slow.lua" "${${2:A}//\"/\\\"}" > "$SCRIPT"
  export SLOW
fi

MACHINE=${MACHINE:-macplus}
BIOS_ARGS=()
[[ "$MACHINE" == macplus ]] && BIOS_ARGS=(-bios v3)
mame $MACHINE "${BIOS_ARGS[@]}" -ramsize ${RAM:-1M} -rompath "$E/roms" "${FLOP1_ARGS[@]}" "${FLOP2_ARGS[@]}" "${HARD_ARGS[@]}" \
  -homepath "$RUN_DIR" -cfg_directory "$RUN_DIR/cfg" -nvram_directory "$RUN_DIR/nvram" -diff_directory "$RUN_DIR/diff" \
  -snapshot_directory "$RUN_DIR/shots" -video none -sound none -nothrottle -skip_gameinfo \
  -seconds_to_run ${3:-120} -autoboot_script "$SCRIPT" ${=MAME_ARGS:-} 2>&1 | grep -v -E "^Average speed|seconds_to_run"

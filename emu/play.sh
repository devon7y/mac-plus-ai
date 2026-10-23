#!/bin/zsh
# Play one of the finished projects in an emulator window.
#   emu/play.sh                 list games and their status
#   emu/play.sh racer           play in a window
#   emu/play.sh --full racer    play fullscreen (Cmd-Q quits)
# Each launch boots from a fresh copy of the game's disk (in emu/runs/play-*), so playing never
# changes the built image. Restart/Shut Down ejects the floppy; it is reinserted automatically.
# games.json fields: disk (floppy 1), flop2, harddisk (SCSI disk, e.g. an SE/System 7 build),
# ram, machine (macplus or macsefd), title, status, controls.
E=${0:A:h}
ROOT=${E:h}
VIDEO=(-window -nomaximize -prescale 2 -resolution 1024x684)
[[ "${1:-}" == "--full" ]] && { VIDEO=(-nowindow -prescale 4); shift; }
GAME=${1:-}

if [[ -z "$GAME" ]]; then
  python3 -I - "$E/games.json" <<'EOF'
import json, sys
g = json.load(open(sys.argv[1]))
print("Games (play with: emu/play.sh <name>):")
for k, v in g.items():
    if k.startswith("_"): continue
    print(f"  {k:13} {v['title']:42} [{v['status']}]")
EOF
  exit 0
fi

eval "$(python3 -I - "$E/games.json" "$GAME" <<'EOF'
import json, shlex, sys
g = json.load(open(sys.argv[1])).get(sys.argv[2])
if not g:
    print("echo 'Unknown game. Run emu/play.sh to list them.'; exit 1"); sys.exit()
if not g.get("disk") and not g.get("harddisk"):
    print(f"echo {shlex.quote(g['title'] + ' is not ready yet (' + g['status'] + ').')}; exit 1"); sys.exit()
print(f"DISK_SRC={shlex.quote(g.get('disk') or '')}; HARD_SRC={shlex.quote(g.get('harddisk') or '')}; FLOP2_SRC={shlex.quote(g.get('flop2') or '')}; "
      f"RAM={shlex.quote(g.get('ram', '1M'))}; MACHINE={shlex.quote(g.get('machine', 'macplus'))}; "
      f"TITLE={shlex.quote(g['title'])}; CONTROLS={shlex.quote(g.get('controls', ''))}")
EOF
)"

RUN=$E/runs/play-$GAME-$(date +%Y%m%d-%H%M%S)
mkdir -p "$RUN/cfg" "$RUN/nvram" "$RUN/diff" "$RUN/shots"
export DISK=""
FLOP1_ARGS=()
if [[ -n "$DISK_SRC" ]]; then DISK="$RUN/disk1.${DISK_SRC:e}"; cp "$ROOT/$DISK_SRC" "$DISK"; FLOP1_ARGS=(-flop1 "$DISK"); fi
HARD_ARGS=()
if [[ -n "$HARD_SRC" ]]; then cp "$ROOT/$HARD_SRC" "$RUN/hard1.${HARD_SRC:e}"; HARD_ARGS=(-hard "$RUN/hard1.${HARD_SRC:e}"); fi
FLOP2_ARGS=()
if [[ -n "$FLOP2_SRC" ]]; then cp "$ROOT/$FLOP2_SRC" "$RUN/disk2.${FLOP2_SRC:e}"; FLOP2_ARGS=(-flop2 "$RUN/disk2.${FLOP2_SRC:e}"); fi
BIOS_ARGS=(); [[ "$MACHINE" == macplus ]] && BIOS_ARGS=(-bios v3)
echo "$TITLE  ($RAM)"
echo "Controls: $CONTROLS"
[[ "$RAM" == 4M && "$MACHINE" == macplus ]] && echo "(A 4 MB Mac tests its memory for ~25 s before starting up.)"
exec mame $MACHINE "${BIOS_ARGS[@]}" -ramsize $RAM -rompath "$E/roms" "${FLOP1_ARGS[@]}" "${FLOP2_ARGS[@]}" "${HARD_ARGS[@]}" \
  -homepath "$RUN" -cfg_directory "$RUN/cfg" -nvram_directory "$RUN/nvram" -diff_directory "$RUN/diff" \
  -snapshot_directory "$RUN/shots" "${VIDEO[@]}" -mouse -skip_gameinfo \
  -ctrlrpath "$E/ctrlr" -ctrlr esc -autoboot_script "$E/scripts/autoreinsert.lua" ${=PLAY_EXTRA:-}

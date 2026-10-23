#!/bin/zsh
# Install System 7.0.1 (Easy Install, as Apple's Installer does it for an SE) onto a fresh
# 20 MB SCSI disk image, fully scripted in MAME (macsefd, 4 MB), then boot it once and shut
# it down from the Finder. About 3 minutes of wall time (about 750 s emulated).
#   toolchain/se7/install_system.sh <out.hd> [emulated seconds, default 1500]
# Needs emu/disks/system-7.0.1/install/*.img (see toolchain/README.md, "System 7 / SE testbed").
set -e
T=${0:A:h}; ROOT=${T:h:h}
OUT=${1:?usage: install_system.sh out.hd [seconds]}
OUT=${OUT:A}
python3 -I $T/make_blank_hd.py --out $OUT
export INSTALL_DIR=$(mktemp -d "${TMPDIR:-/tmp}/se7inst.XXXXXX")
cp $ROOT/emu/disks/system-7.0.1/install/*.img $INSTALL_DIR/ && chmod u+w $INSTALL_DIR/*.img
MACHINE=macsefd RAM=4M HARDDISK=$OUT NO_COPY=1 \
  $ROOT/emu/run_test.sh "$INSTALL_DIR/Install 1.img" $T/install701.lua ${2:-1500}
rm -rf $INSTALL_DIR
# First boot from the hard disk (desktop file), then a clean Shut Down.
MACHINE=macsefd RAM=4M HARDDISK=$OUT NO_COPY=1 $ROOT/emu/run_test.sh none $T/firstboot.lua 400

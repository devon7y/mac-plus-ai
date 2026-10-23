#!/bin/zsh
# Write a floppy image to the USB floppy drive, then read it back to verify.
# Usage: sudo ./write_floppy.sh [image]   (default: NarrowScreen-720K-HFS.img)
# The image must be 737,280 bytes (DD disk, one hole) or 1,474,560 bytes (HD disk,
# two holes). It is only written to a removable disk of exactly that size.
# The disk is found through the I/O registry rather than diskutil, because diskutil
# hangs on unformatted floppies.
set -u
IMG=${1:-${0:A:h}/NarrowScreen-720K-HFS.img}
BYTES=$(stat -f %z "$IMG")
[[ "$BYTES" == 737280 || "$BYTES" == 1474560 ]] || { echo "Image must be 737280 or 1474560 bytes: $IMG"; exit 1; }
TRACK=$((BYTES / 160))                    # 80 cylinders x 2 sides

DEV=$(perl -e 'alarm 10; exec @ARGV' ioreg -r -c IOMedia -d 1 -w0 -l | awk -v want="$BYTES" '
  function flush() { if (bsd != "" && whole == "Yes" && rem == "Yes" && size == want) print bsd, wr; bsd = whole = rem = size = wr = "" }
  /\+-o/            { flush() }
  /"BSD Name" =/    { line = $0; sub(/.*= "/, "", line); sub(/".*/, "", line); bsd = line }
  /"Whole" =/       { whole = $NF }
  /"Removable" =/   { rem = $NF }
  /"Writable" =/    { wr = $NF }
  /"Size" =/        { size = $NF }
  END               { flush() }' | head -1)
WRITABLE=${DEV#* }; DEV=${DEV%% *}
[[ -n "$DEV" ]] || { echo "No floppy of $BYTES bytes found (737280 = DD disk, one hole; 1474560 = HD disk, two holes)."; exit 1; }
[[ "$WRITABLE" == "No" ]] && { echo "The disk is write-protected. Slide its tab to cover the square hole, then try again."; exit 1; }

echo "Writing $(basename "$IMG") to /dev/$DEV ($BYTES bytes) ..."
perl -e 'alarm 20; exec @ARGV' diskutil unmountDisk force "/dev/$DEV" >/dev/null 2>&1
if ! dd if="$IMG" of="/dev/r$DEV" bs=$TRACK; then
  echo "WRITE FAILED. The disk is probably not formatted yet."
  echo "Format it on the SE with Apple File Exchange (File > Erase Disk, MS-DOS 720K), then run this again."
  exit 1
fi
sync
if dd if="/dev/r$DEV" bs=$TRACK count=160 2>/dev/null | cmp -s - "$IMG"; then
  echo "Verified: the floppy matches the image."
else
  echo "VERIFY FAILED: the floppy does not match the image. Try again or use another disk."; exit 1
fi
perl -e 'alarm 20; exec @ARGV' diskutil eject "/dev/$DEV" >/dev/null 2>&1
echo "Done. If macOS says the disk isn't readable, click Eject or Ignore (never Initialize)."

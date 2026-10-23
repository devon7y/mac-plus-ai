#!/usr/bin/env python3
"""Low-level format a floppy in a USB floppy drive on macOS (what Linux's ufiformat does).

macOS has no floppy formatter, and a USB floppy drive can only write sectors that
already exist. This talks to the drive directly over USB (UFI commands over the CBI
transport) and issues FORMAT UNIT track by track, producing a PC-style MFM disk:
720K on a double-density disk, 1.44 MB on a high-density one.

Run as root (macOS only lets root detach the system's driver from the device):
  sudo python3 ufi_format.py --probe        report what the drive sees, change nothing
  sudo python3 ufi_format.py --format       format the disk in the drive (erases it)
  sudo python3 ufi_format.py --write IMG [--tracks 0-85]   write a 720K/1.44M image track by track
  sudo python3 ufi_format.py --verify IMG [--tracks 0-85]  read the disk back and compare with IMG
Writing and verifying go straight to the drive (no /dev/disk node needed), track by track with
retries, and report the tracks that failed instead of stopping at the first error.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "vendor"))
import usb.backend.libusb1  # noqa: E402
import usb.core  # noqa: E402
import usb.util  # noqa: E402

LIBUSB = "/opt/homebrew/lib/libusb-1.0.dylib"
SENSE_KEYS = {0: "no sense", 1: "recovered", 2: "not ready", 3: "medium error", 4: "hardware error",
              5: "illegal request", 6: "unit attention", 7: "write protected"}


class UFIDrive:
    def __init__(self):
        backend = usb.backend.libusb1.get_backend(find_library=lambda _: LIBUSB)
        self.dev = self.intf = None
        for dev in usb.core.find(find_all=True, backend=backend):
            for cfg in dev:
                for intf in cfg:
                    if intf.bInterfaceClass == 8 and intf.bInterfaceSubClass == 4:   # mass storage, UFI
                        self.dev, self.intf = dev, intf
        if self.dev is None:
            sys.exit("No USB floppy (UFI) drive found.")
        self.num = self.intf.bInterfaceNumber
        name = usb.util.get_string(self.dev, self.dev.iProduct) or "?"
        print(f"Drive: {name} (VID {self.dev.idVendor:04x}, PID {self.dev.idProduct:04x}, "
              f"CBI protocol {self.intf.bInterfaceProtocol})")
        if self.dev.is_kernel_driver_active(self.num):
            self.dev.detach_kernel_driver(self.num)
        usb.util.claim_interface(self.dev, self.num)

        def ep(kind, direction):
            return next((e for e in self.intf
                         if usb.util.endpoint_type(e.bmAttributes) == kind
                         and usb.util.endpoint_direction(e.bEndpointAddress) == direction), None)
        self.bulk_out = ep(usb.util.ENDPOINT_TYPE_BULK, usb.util.ENDPOINT_OUT)
        self.bulk_in = ep(usb.util.ENDPOINT_TYPE_BULK, usb.util.ENDPOINT_IN)
        self.intr_in = ep(usb.util.ENDPOINT_TYPE_INTR, usb.util.ENDPOINT_IN)

    def close(self):
        try:
            usb.util.release_interface(self.dev, self.num)
            self.dev.attach_kernel_driver(self.num)          # hand the drive back to macOS
        except usb.core.USBError:
            pass
        usb.util.dispose_resources(self.dev)

    def _cmd(self, cdb, data_out=None, in_len=0, timeout=20000):
        """Send one UFI command. Returns (data, ok)."""
        cdb = bytes(cdb) + bytes(12 - len(cdb))
        try:
            self.dev.ctrl_transfer(0x21, 0x00, 0, self.num, cdb, timeout=timeout)   # ADSC
        except usb.core.USBError:
            return None, False                                 # command rejected (stall)
        data, ok = None, True
        try:
            if data_out:
                self.dev.write(self.bulk_out, data_out, timeout=timeout)
            if in_len:
                data = bytes(self.dev.read(self.bulk_in, in_len, timeout=timeout))
        except usb.core.USBError:
            ok = False
            for e in (self.bulk_out, self.bulk_in):
                try:
                    self.dev.clear_halt(e)
                except usb.core.USBError:
                    pass
        if self.intr_in is not None:                            # CBI status: ASC, ASCQ
            try:
                st = bytes(self.dev.read(self.intr_in, 2, timeout=timeout))
                ok = ok and st[:2] == b"\0\0"
            except usb.core.USBError:
                ok = False
        return data, ok

    def sense(self):
        data, _ = self._cmd([0x03, 0, 0, 0, 18], in_len=18)
        if not data or len(data) < 14:
            return (None, None, None)
        return (data[2] & 0x0F, data[12], data[13])

    def describe_sense(self):
        key, asc, ascq = self.sense()
        return f"{SENSE_KEYS.get(key, key)} (ASC {asc:#04x}/{ascq:#04x})" if key is not None else "no sense data"

    def ready(self, tries=6):
        for _ in range(tries):
            _, ok = self._cmd([0x00])                           # TEST UNIT READY
            if ok:
                return True, "ready"
            msg = self.describe_sense()                         # clears unit attention
            time.sleep(0.5)
        return False, msg

    def format_capacities(self):
        data, ok = self._cmd([0x23, 0, 0, 0, 0, 0, 0, 0, 0xFC], in_len=0xFC)
        if not data or len(data) < 12:
            return []
        out = []
        for i in range(4, 4 + data[3], 8):
            blocks = int.from_bytes(data[i:i + 4], "big")
            code = data[i + 4] & 3
            blksz = int.from_bytes(data[i + 5:i + 8], "big")
            out.append((blocks, blksz, {1: "unformatted (max)", 2: "formatted", 3: "no media"}.get(code, "format")))
        return out

    def write_blocks(self, lba, data):
        n = len(data) // 512
        _, ok = self._cmd([0x2A, 0] + list(lba.to_bytes(4, "big")) + [0] + list(n.to_bytes(2, "big")), data_out=data)
        return ok

    def read_blocks(self, lba, n):
        data, ok = self._cmd([0x28, 0] + list(lba.to_bytes(4, "big")) + [0] + list(n.to_bytes(2, "big")), in_len=n * 512)
        return data if ok and data and len(data) == n * 512 else None

    def format_track(self, track, head, blocks, blksz):
        cdb = [0x04, 0x17, track, 0, 0, 0, 0, 0, 12]            # FORMAT UNIT, FmtData, list format 7
        param = (bytes([0, 0xB0 | head, 0, 8])                  # FOV | DCRT | single track | side
                 + blocks.to_bytes(4, "big") + b"\0" + blksz.to_bytes(3, "big"))
        _, ok = self._cmd(cdb, data_out=param)
        return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", action="store_true", help="report what the drive sees; change nothing")
    ap.add_argument("--format", action="store_true", help="low-level format the disk (erases it)")
    ap.add_argument("--write", metavar="IMG", help="write this image to the disk, track by track")
    ap.add_argument("--verify", metavar="IMG", help="read the disk back and compare with this image")
    ap.add_argument("--tracks", default=None, help="track range, e.g. 0-85 (tracks of 18 or 9 sectors)")
    a = ap.parse_args()
    if os.geteuid() != 0:
        sys.exit("Run with sudo.")
    d = UFIDrive()
    try:
        ok, msg = d.ready()
        print("Status:", msg)
        caps = d.format_capacities()
        for blocks, blksz, kind in caps:
            print(f"  capacity: {blocks} x {blksz} bytes = {blocks * blksz // 1024}K ({kind})")
        if a.write or a.verify:
            img = open(a.write or a.verify, "rb").read()
            if len(img) not in (737280, 1474560):
                sys.exit("Image must be 737,280 or 1,474,560 bytes.")
            spt = len(img) // 512 // 160                          # sectors per track (9 or 18)
            lo, hi = (int(x) for x in a.tracks.split("-")) if a.tracks else (0, 159)
            failed = []
            for t in range(lo, hi + 1):
                lba, chunk = t * spt, img[t * spt * 512:(t + 1) * spt * 512]
                good = False
                for _ in range(3):
                    if a.write and not d.write_blocks(lba, chunk):
                        d.describe_sense(); continue
                    back = d.read_blocks(lba, spt)
                    if back == chunk:
                        good = True; break
                    d.describe_sense()
                if not good:
                    failed.append(t)
                if t % 20 == 19 or t == hi:
                    print(f"  tracks {lo}-{t} done, failed so far: {failed}")
            print("RESULT", "OK" if not failed else f"FAILED tracks {failed}")
            return
        if not a.format:
            return
        maxblocks = caps[0][0] if caps else 0
        if maxblocks not in (1440, 2880):
            sys.exit(f"Unexpected media capacity ({maxblocks} blocks); not formatting.")
        print(f"Formatting as {maxblocks * 512 // 1024}K (80 tracks x 2 sides) ...")
        for track in range(80):
            for head in (0, 1):
                if not d.format_track(track, head, maxblocks, 512):
                    sys.exit(f"Format failed at track {track}, side {head}: {d.describe_sense()}")
            if track % 10 == 9:
                print(f"  tracks 0-{track} done")
        print("Format complete.")
    finally:
        d.close()


if __name__ == "__main__":
    main()

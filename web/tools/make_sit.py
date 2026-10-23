#!/usr/bin/env python3
"""make_sit.py OUT.sit FILE.bin [FILE.bin ...]
Write a classic StuffIt 1.5 archive (stored, no compression) from MacBinary files, keeping type,
creator, Finder flags and both forks. For testing the website's .sit import (StuffIt method 0)."""
import os, struct, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "mactools")); import rsrc

def crc16(data, crc=0):                     # CRC-16/ARC (poly 0xA001 reflected), as StuffIt uses
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc

out, files = sys.argv[1], sys.argv[2:]
mac_epoch = 2082844800
now = int(time.time()) + mac_epoch
body = b''
for f in files:
    hdr, data, rf = rsrc.read_macbinary(f)
    name = hdr[2:2 + hdr[1]]
    h = bytearray(112)
    h[0] = 0; h[1] = 0                                   # rsrc / data method: 0 = stored
    h[2] = len(name); h[3:3 + len(name)] = name
    h[66:74] = hdr[65:73]                                # type + creator
    h[74] = hdr[73]; h[75] = hdr[101]                    # Finder flags hi/lo
    struct.pack_into('>IIIIII', h, 76, now, now, len(rf), len(data), len(rf), len(data))
    struct.pack_into('>HH', h, 100, crc16(rf), crc16(data))
    struct.pack_into('>H', h, 110, crc16(bytes(h[:110])))
    body += bytes(h) + rf + data
arc = b'SIT!' + struct.pack('>HI', len(files), 22 + len(body)) + b'rLau' + bytes([1]) + bytes(7)
open(out, 'wb').write(arc + body)
print(out, len(arc + body), 'bytes,', len(files), 'file(s)')

"""Minimal classic Mac resource fork reader/writer and MacBinary helpers."""
import struct


def read_macbinary(path):
    """Return (header, data_fork, rsrc_fork) from a MacBinary file."""
    mb = open(path, "rb").read()
    dl, rl = struct.unpack(">II", mb[83:91])
    data = mb[128:128 + dl]
    rstart = 128 + ((dl + 127) // 128) * 128
    return mb[:128], data, mb[rstart:rstart + rl]


def parse(fork):
    """Return a list of dicts: type, id, name, attrs, data (in file order)."""
    doff, moff, _, mlen = struct.unpack(">IIII", fork[:16])
    m = fork[moff:moff + mlen]
    tlo, nlo = struct.unpack(">HH", m[24:28])
    ntypes = struct.unpack(">h", m[tlo:tlo + 2])[0] + 1
    out = []
    for i in range(ntypes):
        t, cnt, ref = struct.unpack(">4shH", m[tlo + 2 + 8 * i:tlo + 10 + 8 * i])
        for j in range(cnt + 1):
            rid, noff, ao = struct.unpack(">hhI", m[tlo + ref + 12 * j:tlo + ref + 12 * j + 8])
            off = ao & 0xFFFFFF
            ln = struct.unpack(">I", fork[doff + off:doff + off + 4])[0]
            name = None
            if noff != -1:
                n = m[nlo + noff]
                name = m[nlo + noff + 1:nlo + noff + 1 + n]
            out.append({"type": t, "id": rid, "name": name, "attrs": ao >> 24,
                        "data": fork[doff + off + 4:doff + off + 4 + ln]})
    return out


def build(res):
    """Build a resource fork from a list of resource dicts (as returned by parse)."""
    types = []
    for r in res:
        if r["type"] not in types:
            types.append(r["type"])
    data_area = b""
    names = b""
    refs = {t: [] for t in types}
    for r in res:
        noff = -1
        if r["name"] is not None:
            noff = len(names)
            names += bytes([len(r["name"])]) + r["name"]
        refs[r["type"]].append((r["id"], noff, r["attrs"], len(data_area)))
        data_area += struct.pack(">I", len(r["data"])) + r["data"]
    type_list = struct.pack(">h", len(types) - 1)
    ref_lists = b""
    ref_base = 2 + 8 * len(types)
    for t in types:
        type_list += t + struct.pack(">hH", len(refs[t]) - 1, ref_base + len(ref_lists))
        for rid, noff, attrs, off in refs[t]:
            ref_lists += struct.pack(">hhI", rid, noff, (attrs << 24) | off) + b"\0\0\0\0"
    body = type_list + ref_lists
    tlo, nlo = 28, 28 + len(body)
    mlen = nlo + len(names)
    doff, moff = 256, 256 + len(data_area)
    header = struct.pack(">IIII", doff, moff, len(data_area), mlen)
    rmap = header + b"\0" * 6 + struct.pack(">hHH", 0, tlo, nlo) + body + names
    return header + b"\0" * 240 + data_area + rmap


def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def write_macbinary(path, header, data, rsrc):
    h = bytearray(header)
    h[83:87] = struct.pack(">I", len(data))
    h[87:91] = struct.pack(">I", len(rsrc))
    h[122] = h[123] = 129
    h[124:126] = struct.pack(">H", crc16_xmodem(bytes(h[:124])))
    pad = lambda b: b + b"\0" * ((-len(b)) % 128)
    open(path, "wb").write(bytes(h) + pad(data) + pad(rsrc))

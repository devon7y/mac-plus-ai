#!/usr/bin/env python3
"""Shuttle server: serves a folder on the MacBook to the Shuttle app on a Mac Plus, through a Hayes
Wi-Fi modem (the Plus dials `mac`, the modem opens a TCP connection to this server).

    python3 shuttle/server/shuttle_server.py [--root ~/Shuttle] [--port 6801]

- Serves ~/Shuttle (created on first run). What the Plus sends lands in ~/Shuttle/From Plus/.
- Converts on the fly (see ../PROTOCOL.md, section 4): files with resource forks or Finder info are
  sent as MacBinary; .bin passes through; .sit/.hqx/.sea/.cpt are unpacked with `unar` (Homebrew)
  and offered as the files inside; raw 400K/800K/1440K disk images become Disk Copy 4.2 images;
  text becomes TEXT/ttxt in Mac Roman with CR line ends.
- Python 3.8+, standard library only, plus `unar` for archives (brew install unar).

The protocol (text commands, CRC-checked frames, a sliding window paced by the Mac) is described in
../PROTOCOL.md.
"""
import argparse
import asyncio
import ctypes
import ctypes.util
import hashlib
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
import unicodedata

VERSION = 1
FLAG, ESC, CR = 0x7E, 0x7D, 0x0D
ESCAPED = frozenset((0x0D, 0x11, 0x13, 0x7D, 0x7E, 0xFF))
MAX_BODY = 1024 + 6
MAX_WINDOW, MAX_BLOCK = 8, 1024
SENDER_TIMEOUT = 15.0                  # s without ack/nak before the sender goes back to the oldest block
SENDER_TRIES = 6
RECEIVER_TRIES = 10
MAX_UPLOAD = 32 * 1024 * 1024
MAC_EPOCH = 2082844800                 # seconds from 1904-01-01 to 1970-01-01
ARCHIVE_EXT = (".sit", ".hqx", ".sea", ".cpt")
DISK_EXT = (".dsk", ".img", ".image", ".dc42", ".diskcopy42", ".disk")
TEXT_EXT = (".txt", ".text", ".md", ".markdown", ".c", ".h", ".cpp", ".py", ".s", ".asm", ".p", ".pas",
            ".csv", ".tsv", ".log", ".ini", ".cfg", ".json", ".xml", ".html", ".htm", ".rtf", ".tex",
            ".bas", ".me", ".1st", ".lua", ".sh", ".r")
TYPE_EXT = {".pict": (b"PICT", b"ttxt"), ".pct": (b"PICT", b"ttxt"), ".pntg": (b"PNTG", b"MPNT"),
            ".mac": (b"PNTG", b"MPNT")}
RAW_DISKS = {409600: (0, 0x12, 400 * 24), 819200: (1, 0x22, 1600 * 12), 1474560: (3, 0x22, 0)}


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


# ------------------------------------------------------------------------------------------ CRC
def _crc_table():
    t = []
    for i in range(256):
        c = i << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else (c << 1)
        t.append(c & 0xFFFF)
    return t


CRC_TABLE = _crc_table()


def crc16(data, crc=0):
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ CRC_TABLE[((crc >> 8) ^ b) & 0xFF]
    return crc


# ------------------------------------------------------------------------------------------ frames
def encode_frame(ftype, tid, block, payload=b""):
    body = bytes((ftype, tid & 0xFF, (block >> 8) & 0xFF, block & 0xFF)) + payload
    c = crc16(body)
    body += bytes((c >> 8, c & 0xFF))
    out = bytearray((FLAG,))
    for b in body:
        if b in ESCAPED:
            out += bytes((ESC, b ^ 0x20))
        else:
            out.append(b)
    out.append(CR)
    return bytes(out)


class Decoder:
    """Splits the byte stream into text lines and frames (PROTOCOL.md section 1)."""

    def __init__(self):
        self.mode = 0                   # 0 text, 1 frame, 2 frame after ESC
        self.line = bytearray()
        self.body = bytearray()

    def feed(self, data):
        out = []
        for b in data:
            if self.mode == 0:
                if b == FLAG:
                    self.mode, self.body = 1, bytearray()
                    self.line = bytearray()
                elif b == CR:
                    out.append(("line", bytes(self.line)))
                    self.line = bytearray()
                elif b not in (0, 10) and len(self.line) < 255:
                    self.line.append(b)
            else:
                if b == FLAG:
                    self.mode, self.body = 1, bytearray()
                elif b == CR:
                    self.mode = 0
                    out.append(self._frame())
                elif self.mode == 2:
                    self.mode = 1
                    self.body.append(b ^ 0x20)
                elif b == ESC:
                    self.mode = 2
                else:
                    self.body.append(b)
                if len(self.body) > MAX_BODY:
                    self.mode, self.body = 0, bytearray()
                    out.append(("bad",))
        return out

    def _frame(self):
        b = bytes(self.body)
        if len(b) < 6 or crc16(b[:-2]) != (b[-2] << 8 | b[-1]):
            return ("bad",)
        return ("frame", b[0], b[1], b[2] << 8 | b[3], b[4:-2])


# ------------------------------------------------------------------------------------------ Mac metadata
_libc = None


def _xattr_lib():
    global _libc
    if _libc is None:
        _libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
        _libc.getxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.c_uint32, ctypes.c_int]
        _libc.getxattr.restype = ctypes.c_ssize_t
        _libc.setxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.c_uint32, ctypes.c_int]
        _libc.setxattr.restype = ctypes.c_int
    return _libc


def get_finder_info(path):
    """The 32-byte com.apple.FinderInfo of a file, or None."""
    if sys.platform != "darwin":
        return None
    lib = _xattr_lib()
    buf = ctypes.create_string_buffer(32)
    n = lib.getxattr(os.fsencode(path), b"com.apple.FinderInfo", buf, 32, 0, 0)
    return buf.raw[:32] if n >= 16 else None


def set_finder_info(path, info):
    if sys.platform != "darwin":
        return False
    lib = _xattr_lib()
    info = (info + bytes(32))[:32]
    return lib.setxattr(os.fsencode(path), b"com.apple.FinderInfo", info, 32, 0, 0) == 0


def rsrc_path(path):
    return os.path.join(path, "..namedfork", "rsrc")


def rsrc_size(path):
    if sys.platform != "darwin":
        return 0
    try:
        return os.path.getsize(rsrc_path(path))
    except OSError:
        return 0


def read_rsrc(path):
    if not rsrc_size(path):
        return b""
    with open(rsrc_path(path), "rb") as f:
        return f.read()


# ------------------------------------------------------------------------------------------ MacBinary
def pad128(n):
    return (n + 127) & ~127


def mac_time(t):
    return max(0, min(0xFFFFFFFF, int(t) + MAC_EPOCH))


def macbinary(name, ftype, creator, flags, data, rsrc, ctime=None, mtime=None):
    """A MacBinary II file: header, data fork, resource fork (each padded to 128 bytes)."""
    now = time.time()
    h = bytearray(128)
    h[1] = len(name)
    h[2:2 + len(name)] = name
    h[65:69], h[69:73] = ftype, creator
    h[73] = (flags >> 8) & 0xFF
    struct.pack_into(">II", h, 83, len(data), len(rsrc))
    struct.pack_into(">II", h, 91, mac_time(ctime or now), mac_time(mtime or now))
    h[101] = flags & 0xFF
    h[122] = h[123] = 129
    struct.pack_into(">H", h, 124, crc16(bytes(h[:124])))
    return (bytes(h) + data + bytes(pad128(len(data)) - len(data)) + rsrc
            + bytes(pad128(len(rsrc)) - len(rsrc)))


def parse_macbinary(b):
    """(name, type, creator, flags, data, rsrc, ctime, mtime) or None if b isn't MacBinary."""
    if len(b) < 128 or b[0] != 0 or b[74] != 0 or b[82] != 0 or not 1 <= b[1] <= 63:
        return None
    dlen, rlen = struct.unpack(">II", b[83:91])
    if dlen > 0x7FFFFFFF or rlen > 0x7FFFFF or len(b) < 128 + pad128(dlen) + rlen:
        return None
    if b[122] >= 129 and struct.unpack(">H", b[124:126])[0] not in (0, crc16(b[:124])):
        return None
    if b[122] < 129 and any(b[99:126]):
        return None                                  # MacBinary I: those bytes are zero
    ctime, mtime = struct.unpack(">II", b[91:99])
    flags = b[73] << 8 | (b[101] if b[122] >= 129 else 0)
    return (bytes(b[2:2 + b[1]]), bytes(b[65:69]), bytes(b[69:73]), flags, bytes(b[128:128 + dlen]),
            bytes(b[128 + pad128(dlen):128 + pad128(dlen) + rlen]), ctime, mtime)


def dc42(raw, name):
    """A Disk Copy 4.2 image of a raw 400K/800K/1440K disk (zero tags, as for an HFS disk)."""
    fmt, fbyte, tags = RAW_DISKS[len(raw)]

    def ck(b):
        s = 0
        for i in range(0, len(b) - 1, 2):
            s = (s + (b[i] << 8 | b[i + 1])) & 0xFFFFFFFF
            s = ((s >> 1) | (s << 31)) & 0xFFFFFFFF
        return s
    nm = name[:63]
    h = bytearray(84)
    h[0] = len(nm)
    h[1:1 + len(nm)] = nm
    struct.pack_into(">IIII", h, 0x40, len(raw), tags, ck(raw), 0)
    h[0x50], h[0x51] = fmt, fbyte
    struct.pack_into(">H", h, 0x52, 0x0100)
    return bytes(h) + raw + bytes(tags)


def is_dc42(path, size):
    if size < 84 + 409600:
        return False
    with open(path, "rb") as f:
        h = f.read(84)
    dlen, tlen = struct.unpack(">II", h[0x40:0x48])
    return h[0] <= 63 and h[0x52:0x54] == b"\x01\x00" and 84 + dlen + tlen == size


# ------------------------------------------------------------------------------------------ names
def mac_name(posix_name, limit=31):
    """A host file name as a Mac name: Mac Roman, at most 31 bytes, ':' (Finder's '/') back to '/'."""
    s = unicodedata.normalize("NFC", posix_name).replace(":", "/")
    b = s.encode("mac_roman", errors="replace")
    if len(b) > limit:
        stem, dot, ext = s.rpartition(".")
        ext_b = ("." + ext).encode("mac_roman", errors="replace") if dot and len(ext) <= 5 else b""
        b = b[:limit - 1 - len(ext_b)] + b"\xc9" + ext_b          # ... (Mac Roman ellipsis)
    return b


def posix_name(mac):
    s = mac.decode("mac_roman", errors="replace").replace("/", ":").strip() or "Untitled"
    return s.replace("\0", "")


def text_to_mac(data):
    s = data.decode("utf-8")
    s = s.replace("\r\n", "\r").replace("\n", "\r")
    repl = {"‐": "-", "‑": "-", "‒": "-", "–": "\xd0", "—": "\xd1",
            "→": "->", "←": "<-", " ": " ", "\t": "    "}
    s = "".join(repl.get(c, c) for c in unicodedata.normalize("NFC", s))
    return s.encode("mac_roman", errors="replace")


def looks_text(path, size):
    if size > 4 * 1024 * 1024:
        return False
    with open(path, "rb") as f:
        b = f.read()
    if b"\0" in b:
        return False
    try:
        b.decode("utf-8")
        return True
    except UnicodeDecodeError:
        return False


# ------------------------------------------------------------------------------------------ the served tree
class Entry:
    """One item of a listing: kind 'D' folder, 'A' archive, 'F' file."""

    def __init__(self, kind, name, path, **kw):
        self.kind, self.name, self.path = kind, name, path
        self.count = kw.get("count", 0)
        self.size = kw.get("size", 0)
        self.dlen = kw.get("dlen", 0)
        self.rlen = kw.get("rlen", 0)
        self.ftype = kw.get("ftype", b"????")
        self.creator = kw.get("creator", b"????")
        self.make = kw.get("make")                  # () -> MacBinary bytes

    def line(self):
        if self.kind == "D":
            return b"D %d %s" % (self.count, self.name)
        if self.kind == "A":
            return b"A %d %s" % (self.size, self.name)
        return b"F %d %d %s %s %s" % (self.dlen, self.rlen, self.ftype.hex().upper().encode(),
                                      self.creator.hex().upper().encode(), self.name)


class Tree:
    def __init__(self, root, cache):
        self.root = root
        self.cache = cache

    # ---- archives
    def unpack(self, path):
        """Unpack an archive into the cache (once per version of the file); the folder or an error."""
        st = os.stat(path)
        key = hashlib.sha1(("%s|%d|%d" % (os.path.abspath(path), st.st_size, st.st_mtime_ns)).encode()).hexdigest()[:16]
        out = os.path.join(self.cache, key)
        if os.path.isdir(out) and os.path.exists(os.path.join(out, ".shuttle_ok")):
            return out, None
        tmp = out + ".part%d" % os.getpid()
        os.makedirs(tmp, exist_ok=True)
        unar = shutil.which("unar") or "/opt/homebrew/bin/unar"
        if not os.path.exists(unar):
            return None, "unar is not installed on the MacBook (brew install unar)"
        try:
            r = subprocess.run([unar, "-q", "-f", "-D", "-k", "fork", "-nq", "-o", tmp, path],
                               capture_output=True, text=True, timeout=300)
        except subprocess.TimeoutExpired:
            return None, "unpacking took too long"
        if r.returncode != 0 or not os.listdir(tmp):
            msg = (r.stderr or r.stdout or "").strip().splitlines()
            return None, "unar couldn't unpack it" + (": " + msg[-1][:80] if msg else "")
        open(os.path.join(tmp, ".shuttle_ok"), "w").close()
        if os.path.isdir(out):                       # a half-made folder from an older run: set aside
            os.rename(out, out + ".old%d" % int(time.time()))
        os.rename(tmp, out)
        return out, None

    # ---- one host file -> Entry
    def file_entry(self, path, name, st):
        low = name.lower()
        size = st.st_size
        fi = get_finder_info(path)
        rlen = rsrc_size(path)
        ftype, creator = (fi[0:4], fi[4:8]) if fi else (b"\0\0\0\0", b"\0\0\0\0")
        flags = (fi[8] << 8 | fi[9]) if fi else 0
        has_type = ftype != b"\0\0\0\0"
        mname = mac_name(name)

        if low.endswith(ARCHIVE_EXT) or low.endswith(tuple(x + ".bin" for x in ARCHIVE_EXT)):
            return Entry("A", mname, path, size=size + rlen)
        if low.endswith(".bin") and not rlen and size >= 128:
            with open(path, "rb") as f:
                head = f.read(128)
            if head[0] == 0 and head[74] == 0 and head[82] == 0 and 1 <= head[1] <= 63:
                dl, rl = struct.unpack(">II", head[83:91])
                if size >= 128 + pad128(dl) + rl and (head[122] < 129 or struct.unpack(">H", head[124:126])[0]
                                                       in (0, crc16(head[:124]))):
                    def make_bin(p=path):
                        with open(p, "rb") as f:
                            return f.read()
                    return Entry("F", mac_name(head[2:2 + head[1]].decode("mac_roman").replace("/", ":")), path,
                                 dlen=dl, rlen=rl, ftype=head[65:69], creator=head[69:73], make=make_bin)
        if has_type or rlen:
            def make_native(p=path, nm=mname, t=ftype, c=creator, fl=flags, s=st):
                with open(p, "rb") as f:
                    data = f.read()
                return macbinary(nm, t, c, fl, data, read_rsrc(p), s.st_birthtime if hasattr(s, "st_birthtime")
                                 else s.st_ctime, s.st_mtime)
            if not has_type:
                ftype, creator = b"????", b"????"
            return Entry("F", mname, path, dlen=size, rlen=rlen, ftype=ftype, creator=creator, make=make_native)
        if low.endswith(DISK_EXT) or size in RAW_DISKS:
            if is_dc42(path, size):
                def make_img(p=path, nm=mname, s=st):
                    with open(p, "rb") as f:
                        return macbinary(nm, b"dImg", b"dCpy", 0, f.read(), b"", None, s.st_mtime)
                return Entry("F", mname, path, dlen=size, ftype=b"dImg", creator=b"dCpy", make=make_img)
            if size in RAW_DISKS and low.endswith(DISK_EXT):
                fmt, fb, tags = RAW_DISKS[size]
                disk_name = name.rsplit(".", 1)[0]

                def make_raw(p=path, nm=mname, dn=disk_name, s=st):
                    with open(p, "rb") as f:
                        img = dc42(f.read(), dn.encode("mac_roman", errors="replace"))
                    return macbinary(nm, b"dImg", b"dCpy", 0, img, b"", None, s.st_mtime)
                return Entry("F", mname, path, dlen=84 + size + tags, ftype=b"dImg", creator=b"dCpy", make=make_raw)
        ext = os.path.splitext(low)[1]
        if ext in TYPE_EXT:
            t, c = TYPE_EXT[ext]

            def make_typed(p=path, nm=mname, t=t, c=c, s=st):
                with open(p, "rb") as f:
                    return macbinary(nm, t, c, 0, f.read(), b"", None, s.st_mtime)
            return Entry("F", mname, path, dlen=size, ftype=t, creator=c, make=make_typed)
        if ext in TEXT_EXT and looks_text(path, size):
            with open(path, "rb") as f:
                text = text_to_mac(f.read())

            def make_text(nm=mname, text=text, s=st):
                return macbinary(nm, b"TEXT", b"ttxt", 0, text, b"", None, s.st_mtime)
            return Entry("F", mname, path, dlen=len(text), ftype=b"TEXT", creator=b"ttxt", make=make_text)

        def make_plain(p=path, nm=mname, s=st):
            with open(p, "rb") as f:
                return macbinary(nm, b"????", b"????", 0, f.read(), b"", None, s.st_mtime)
        return Entry("F", mname, path, dlen=size, make=make_plain)

    # ---- a folder -> entries
    def list_dir(self, d):
        entries = []
        try:
            names = os.listdir(d)
        except OSError:
            return []
        for name in names:
            if name.startswith(".") or any(ord(c) < 32 for c in name) or name == "Icon\r":
                continue
            p = os.path.join(d, name)
            try:
                st = os.stat(p)
            except OSError:
                continue
            if os.path.isdir(p):
                try:
                    n = sum(1 for x in os.listdir(p) if not x.startswith(".") and not any(ord(c) < 32 for c in x))
                except OSError:
                    n = 0
                entries.append(Entry("D", mac_name(name), p, count=n))
            elif os.path.isfile(p):
                try:
                    entries.append(self.file_entry(p, name, st))
                except (OSError, ValueError, UnicodeDecodeError) as e:
                    log("skipped", p, e)
        entries.sort(key=lambda e: (e.kind == "F", e.name.decode("mac_roman").lower()))
        seen = set()
        for e in entries:                             # unique names within the folder
            base, k = e.name, 2
            while e.name.lower() in seen:
                suffix = b" %d" % k
                e.name = base[:31 - len(suffix)] + suffix
                k += 1
            seen.add(e.name.lower())
        return entries

    def resolve(self, mac_path):
        """('dir', host folder) or ('file', Entry) for a Mac path; raises LookupError(code, message)."""
        parts = [p for p in mac_path.split(b":") if p]
        cur = self.root
        for i, part in enumerate(parts):
            match = None
            for e in self.list_dir(cur):
                if e.name == part:
                    match = e
                    break
            if match is None:
                raise LookupError("NOTFOUND", "%s isn't there any more" % part.decode("mac_roman"))
            last = i == len(parts) - 1
            if match.kind == "F":
                if not last:
                    raise LookupError("NOTFOUND", "%s is a file" % part.decode("mac_roman"))
                return "file", match
            if match.kind == "A":
                out, err = self.unpack(match.path)
                if err:
                    raise LookupError("BADARCHIVE", err)
                cur = out
            else:
                cur = match.path
        return "dir", cur


# ------------------------------------------------------------------------------------------ transfers
class Sender:
    """Sends one transfer (a listing or a file) as data frames with a sliding window (go-back-N)."""

    def __init__(self, sess, tid, data, cmdno, what):
        self.sess, self.tid, self.data, self.cmdno, self.what = sess, tid, data, cmdno, what
        self.B, self.W = sess.block, sess.window
        self.n = (len(data) + self.B - 1) // self.B
        self.base = self.nxt = 0
        self.progress = time.monotonic()
        self.timeouts = 0
        self.resends = 0
        self.t0 = time.monotonic()

    def resolve(self, v):
        d = (v - self.base) & 0xFFFF
        return self.base + d if d <= self.nxt - self.base else None

    def pump(self):
        while self.nxt < self.n and self.nxt < self.base + self.W:
            k = self.nxt
            self.sess.write(encode_frame(0x44, self.tid, k & 0xFFFF, self.data[k * self.B:(k + 1) * self.B]))
            self.nxt += 1
        if self.base >= self.n:
            self.sess.finish_send(self)

    def ack(self, v, nak=False):
        full = self.resolve(v)
        if full is None:
            return
        if full > self.base or nak:
            self.progress = time.monotonic()
            self.timeouts = 0
        if full > self.base:
            self.base = full
        if nak and full < self.nxt:
            self.resends += self.nxt - full
            self.nxt = full
        self.pump()

    def tick(self):
        if self.base < self.n and time.monotonic() - self.progress > SENDER_TIMEOUT:
            self.timeouts += 1
            if self.timeouts > SENDER_TRIES:
                return False
            log("  no answer from the Mac for %.0f s: sending again from block %d" % (SENDER_TIMEOUT, self.base))
            self.progress = time.monotonic()
            self.resends += self.nxt - self.base
            self.nxt = self.base
            self.pump()
        return True


class Receiver:
    """Receives an upload (the Mac sends), acking in order, nak on gaps (PROTOCOL.md section 2)."""

    def __init__(self, sess, tid, size, name, cmdno):
        self.sess, self.tid, self.size, self.name, self.cmdno = sess, tid, size, name, cmdno
        self.B = sess.block
        self.n = (size + self.B - 1) // self.B
        self.next = 0
        self.buf = bytearray()
        self.progress = time.monotonic()
        self.last_nak = (None, 0.0)
        self.seen = -1                     # highest block number seen
        self.nak_seen = -1                 # ... when the last nak was sent
        self.timeouts = 0
        self.saved = None                  # the final reply, once the file is saved
        self.t0 = time.monotonic()

    def nak(self, force=False, full=None):
        now = time.monotonic()
        v, t = self.last_nak
        rewound = full is not None and full <= self.nak_seen      # the sender went back: our block was lost again
        if force or rewound or v != self.next or now - t > self.sess.nak_wait:
            self.sess.write(encode_frame(0x4E, self.tid, self.next & 0xFFFF))
            self.last_nak = (self.next, now)
            self.nak_seen = self.seen

    def data(self, block, payload):
        d = ((block - self.next + 0x8000) & 0xFFFF) - 0x8000
        full = self.next + d
        rewound = full <= self.nak_seen
        self.seen = max(self.seen, full)
        if d == 0 and self.next < self.n:
            want = min(self.B, self.size - self.next * self.B)
            if len(payload) != want:
                self.nak()
                return
            self.buf += payload
            self.next += 1
            self.progress = time.monotonic()
            self.timeouts = 0
            self.sess.write(encode_frame(0x41, self.tid, self.next & 0xFFFF))
            if self.next == self.n:
                self.sess.finish_upload(self)
        elif d < 0 or self.next >= self.n:
            self.sess.write(encode_frame(0x41, self.tid, self.next & 0xFFFF))
            if self.saved:
                self.sess.text(self.saved, self.cmdno)
        else:
            self.nak(full=full if rewound else None)

    def tick(self):
        if self.next < self.n and time.monotonic() - self.progress > self.sess.nak_wait + 2:
            self.timeouts += 1
            if self.timeouts > RECEIVER_TRIES:
                return False
            self.progress = time.monotonic()
            self.nak(force=True)
        return True


# ------------------------------------------------------------------------------------------ a connection
class Session:
    def __init__(self, server, reader, writer):
        self.srv, self.reader, self.writer = server, reader, writer
        self.dec = Decoder()
        self.window, self.block = 4, 1024
        self.nak_wait = 4.0
        self.sender = None
        self.receiver = None
        self.finished = None               # (tid, cmdno, "DONE n") of the last finished download
        self.tid = 0
        self.last_cmd = None               # (command number, reply) for retried commands
        self.closing = False
        self.peer = writer.get_extra_info("peername")

    def write(self, b):
        if not self.closing:
            self.writer.write(b)

    def text(self, s, cmdno):
        """A reply: an R frame answering command cmdno, or a plain text line if cmdno is None."""
        if isinstance(s, str):
            s = s.encode("mac_roman", errors="replace")
        if cmdno is None:
            self.write(s + b"\r")
        else:
            self.write(encode_frame(0x52, 0, cmdno & 0xFFFF, s))

    def next_tid(self):
        self.tid = self.tid % 255 + 1
        return self.tid

    def stop_transfer(self):
        self.sender = None
        self.receiver = None

    async def run(self):
        log("connected:", self.peer)
        self.write(b"\rSHUTTLE %d %s\r" % (VERSION, self.srv.name.encode("mac_roman", errors="replace")))
        ticker = asyncio.ensure_future(self.ticks())
        last_rx = time.monotonic()
        try:
            while not self.closing:
                try:
                    data = await asyncio.wait_for(self.reader.read(4096), timeout=60)
                except asyncio.TimeoutError:
                    if time.monotonic() - last_rx > self.srv.idle:
                        log("idle for %d s: closing" % self.srv.idle)
                        break
                    continue
                if not data:
                    break
                last_rx = time.monotonic()
                for item in self.dec.feed(data):
                    await self.handle(item)
                await self.writer.drain()
        except (ConnectionError, OSError):
            pass
        finally:
            ticker.cancel()
            log("disconnected:", self.peer)
            try:
                self.writer.close()
            except Exception:
                pass

    async def ticks(self):
        while True:
            await asyncio.sleep(1.0)
            if self.sender and not self.sender.tick():
                log("  the Mac stopped answering: transfer abandoned")
                self.sender = None
            if self.receiver and not self.receiver.tick():
                log("  the Mac stopped sending: upload abandoned")
                self.receiver = None
            try:
                await self.writer.drain()
            except (ConnectionError, OSError):
                return

    async def handle(self, item):
        if item[0] == "line":
            s = item[1].strip()
            if s and not s.startswith(b"SHUTTLE"):
                await self.command(s, None)
        elif item[0] == "frame":
            _, ftype, tid, block, payload = item
            if ftype == 0x43:                                  # C: a command
                if self.last_cmd and self.last_cmd[0] == block:
                    log("  (command %d again: the reply was lost)" % block)
                    self.text(self.last_cmd[1], block)
                else:
                    await self.command(payload.strip(), block)
            elif ftype in (0x41, 0x4E):
                if self.sender and tid == self.sender.tid:
                    self.sender.ack(block, nak=ftype == 0x4E)
                elif self.finished and tid == self.finished[0]:
                    self.text(self.finished[2], self.finished[1])  # the Mac missed DONE
            elif ftype == 0x44 and self.receiver and tid == self.receiver.tid:
                self.receiver.data(block, payload)
        else:
            self.srv.bad_frames += 1
            if self.receiver:
                self.receiver.nak()

    async def slow(self, what, fn, *args):
        """Run file work in a thread; tell the Mac what's happening every 3 s so it keeps waiting."""
        loop = asyncio.get_running_loop()
        fut = loop.run_in_executor(None, fn, *args)
        while True:
            done, _ = await asyncio.wait([fut], timeout=3.0)
            if done:
                return fut.result()
            self.text("NOTE %s" % what, self.cur)
            await self.writer.drain()

    # ---- commands
    async def command(self, s, cmdno):
        word, _, rest = s.partition(b" ")
        cmd = word.upper()
        self.cur = cmdno
        if cmd != b"PING":
            log("<", s.decode("mac_roman", errors="replace")[:120])
        if cmd not in (b"CANCEL", b"PING") and (self.sender or self.receiver):
            self.stop_transfer()                               # a new command ends a transfer
        reply = None
        try:
            if cmd == b"HELLO":
                a = rest.split()
                if not a or a[0] != b"1":
                    reply = "ERR VERSION this server speaks Shuttle protocol 1"
                else:
                    w = int(a[1]) if len(a) > 1 else 4
                    b = int(a[2]) if len(a) > 2 else 1024
                    bps = int(a[3]) if len(a) > 3 else 1900
                    self.window = max(1, min(MAX_WINDOW, w))
                    self.block = max(64, min(MAX_BLOCK, b))
                    self.nak_wait = self.window * self.block / max(100, bps) + 1.0
                    reply = "OK HELLO 1 %d %d %s" % (self.window, self.block, self.srv.name)
            elif cmd == b"LIST":
                path = rest.strip()

                def work():
                    kind, d = self.srv.tree.resolve(path)
                    if kind != "dir":
                        raise LookupError("NOTFOUND", "that is a file, not a folder")
                    return self.srv.tree.list_dir(d)
                entries = await self.slow("opening %s\311" % (path.decode("mac_roman") or "the folder"), work)
                data = b"".join(e.line() + b"\r" for e in entries)
                tid = self.next_tid()
                reply = "OK LIST %d %d %d" % (tid, len(data), len(entries))
                self.reply(cmdno, reply)
                self.start_send(tid, data, cmdno, "listing of %s (%d items)" % (path.decode("mac_roman") or ":",
                                                                             len(entries)))
                return
            elif cmd == b"GET":
                path = rest.strip()

                def work():
                    kind, e = self.srv.tree.resolve(path)
                    if kind != "file":
                        raise LookupError("NOTFOUND", "that is a folder")
                    return e, e.make()
                e, data = await self.slow("reading %s\311" % path.rpartition(b":")[2].decode("mac_roman"), work)
                mb = parse_macbinary(data)
                if not mb:
                    reply = "ERR IO couldn't read that file"
                else:
                    tid = self.next_tid()
                    reply = "OK GET %d %d %d %d" % (tid, len(data), len(mb[4]), len(mb[5]))
                    self.reply(cmdno, reply)
                    self.start_send(tid, data, cmdno, "%s (%d + %d bytes)" % (
                        e.name.decode("mac_roman"), len(mb[4]), len(mb[5])))
                    return
            elif cmd == b"PUT":
                name, _, size = rest.rpartition(b" ")
                size = int(size)
                if size > MAX_UPLOAD:
                    reply = "ERR TOOBIG the server takes files up to 32 MB"
                elif size < 128:
                    reply = "ERR BADFILE that isn't a MacBinary file"
                else:
                    tid = self.next_tid()
                    self.receiver = Receiver(self, tid, size, name, cmdno)
                    log("  receiving %s: %d bytes" % (name.decode("mac_roman"), size))
                    reply = "OK PUT %d" % tid
            elif cmd == b"CANCEL":
                if self.sender or self.receiver:
                    log("  cancelled by the Mac")
                self.stop_transfer()
                reply = "OK CANCEL"
            elif cmd == b"PING":
                reply = "OK PING"
            elif cmd == b"BYE":
                self.reply(cmdno, "OK BYE")
                await self.writer.drain()
                self.closing = True
                return
            else:
                reply = "ERR BADCMD unknown command"
        except LookupError as ex:
            code, msg = ex.args if len(ex.args) == 2 else ("NOTFOUND", str(ex))
            reply = "ERR %s %s" % (code, msg)
        except ValueError:
            reply = "ERR BADCMD bad arguments"
        except OSError as ex:
            reply = "ERR IO %s" % str(ex.strerror or ex)[:100]
        self.reply(cmdno, reply)

    def reply(self, cmdno, reply):
        if reply.startswith("ERR"):
            log(">", reply)
        if cmdno is not None:
            self.last_cmd = (cmdno, reply)
        self.text(reply, cmdno)

    def start_send(self, tid, data, cmdno, what):
        self.sender = Sender(self, tid, data, cmdno, what)
        log("  sending %s: %d bytes in %d blocks" % (what, len(data), self.sender.n))
        self.sender.pump()

    def finish_send(self, snd):
        if self.sender is not snd:
            return
        dt = time.monotonic() - snd.t0
        log("  sent %s: %d bytes in %.1f s (%d B/s), %d blocks resent" % (
            snd.what, len(snd.data), dt, len(snd.data) / dt if dt > 0 else 0, snd.resends))
        self.finished = (snd.tid, snd.cmdno, "DONE %d" % snd.tid)
        self.text(self.finished[2], snd.cmdno)
        self.sender = None

    def finish_upload(self, rcv):
        mb = parse_macbinary(bytes(rcv.buf))
        if not mb:
            rcv.saved = b"ERR BADFILE that wasn't a MacBinary file"
            self.text(rcv.saved, rcv.cmdno)
            return
        try:
            saved = self.srv.save_upload(mb)
        except OSError as ex:
            rcv.saved = ("ERR IO %s" % str(ex.strerror or ex)[:100]).encode()
            self.text(rcv.saved, rcv.cmdno)
            return
        dt = time.monotonic() - rcv.t0
        log("  received %s: %d bytes in %.1f s (%d B/s) -> %s" % (
            mb[0].decode("mac_roman"), rcv.size, dt, rcv.size / dt if dt > 0 else 0, saved))
        rcv.saved = b"OK SAVED %d %s" % (rcv.tid, mac_name(os.path.basename(saved)))
        self.text(rcv.saved, rcv.cmdno)


class Server:
    def __init__(self, root, cache, name, idle):
        self.root = root
        self.inbox = os.path.join(root, "From Plus")
        self.tree = Tree(root, cache)
        self.name = name
        self.idle = idle
        self.bad_frames = 0

    def save_upload(self, mb):
        name, ftype, creator, flags, data, rsrc, ctime, mtime = mb
        os.makedirs(self.inbox, exist_ok=True)
        base = posix_name(name)
        path, k = os.path.join(self.inbox, base), 2
        while os.path.lexists(path):
            stem, dot, ext = base.rpartition(".")
            path = os.path.join(self.inbox, ("%s %d.%s" % (stem, k, ext)) if dot and stem else "%s %d" % (base, k))
            k += 1
        with open(path, "wb") as f:
            f.write(data)
        native = sys.platform == "darwin"
        if native and rsrc:
            try:
                with open(rsrc_path(path), "wb") as f:
                    f.write(rsrc)
            except OSError:
                native = False
        if native:
            info = ftype + creator + struct.pack(">H", flags & ~0x0100 & 0xFFFF) + bytes(22)
            native = set_finder_info(path, info)
        if not native and (rsrc or ftype not in (b"TEXT", b"????")):
            os.replace(path, path + ".partial")                  # keep everything: write MacBinary instead
            binpath = path + ".bin"
            with open(binpath, "wb") as f:
                f.write(macbinary(name, ftype, creator, flags, data, rsrc, None, None))
            os.replace(path + ".partial", path)
            path = binpath
        if MAC_EPOCH < mtime < 0xFFFFFFFF:
            try:
                os.utime(path, (time.time(), mtime - MAC_EPOCH))
            except OSError:
                pass
        return path


def lan_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))             # no packet is sent: this just picks the interface
        ip = s.getsockname()[0]
        s.close()
        if not ip.startswith("127."):
            return ip
    except OSError:
        pass
    for iface in ("en0", "en1"):
        try:
            ip = subprocess.run(["ipconfig", "getifaddr", iface], capture_output=True, text=True).stdout.strip()
            if ip:
                return ip
        except OSError:
            pass
    return "127.0.0.1"


def main():
    ap = argparse.ArgumentParser(description="Shuttle server: serves a folder to the Shuttle app on a Mac Plus")
    ap.add_argument("--root", default=os.path.expanduser("~/Shuttle"), help="folder to serve (default ~/Shuttle)")
    ap.add_argument("--port", type=int, default=6801)
    ap.add_argument("--host", default="0.0.0.0", help="address to listen on (default: all)")
    ap.add_argument("--cache", default=os.path.expanduser("~/Library/Caches/Shuttle"),
                    help="where archives are unpacked")
    ap.add_argument("--name", default=socket.gethostname().split(".")[0][:31], help="server name shown on the Plus")
    ap.add_argument("--idle", type=int, default=1800, help="hang up after this many seconds of silence")
    a = ap.parse_args()
    root = os.path.abspath(os.path.expanduser(a.root))
    if not os.path.isdir(root):
        os.makedirs(root)
        with open(os.path.join(root, "Welcome to Shuttle.txt"), "w") as f:
            f.write("Put files for your Mac Plus in this folder (%s).\nFiles the Plus sends arrive in "
                    "\"From Plus\".\n" % root)
    os.makedirs(os.path.join(root, "From Plus"), exist_ok=True)
    os.makedirs(a.cache, exist_ok=True)
    srv = Server(root, a.cache, a.name.replace(" ", "-") or "MacBook", a.idle)

    async def run():
        async def on_conn(reader, writer):
            sock = writer.get_extra_info("socket")
            if sock is not None:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            await Session(srv, reader, writer).run()
        server = await asyncio.start_server(on_conn, a.host, a.port)
        ip = lan_ip()
        print("Shuttle server %d.0, serving %s" % (VERSION, root))
        print("  Files the Plus sends go to %s" % srv.inbox)
        print("  Listening on port %d. This MacBook's address on the network: %s" % (a.port, ip))
        print()
        print("  Tell the modem once where the MacBook is (Shuttle > Connection > Modem Terminal on the Plus,")
        print("  or python3 modem/tools/console.py on the MacBook with the modem on USB):")
        print()
        print("      AT+ALIAS mac=%s:%d" % (ip, a.port))
        print()
        print("  The first time, macOS may ask whether Python may accept incoming network connections: click Allow.")
        print("  If the MacBook's address changes (another network), type the AT+ALIAS line again.")
        print("  Press Control-C to stop.", flush=True)
        async with server:
            await server.serve_forever()
    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        print()


if __name__ == "__main__":
    main()

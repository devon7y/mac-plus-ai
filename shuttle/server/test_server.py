#!/usr/bin/env python3
"""Offline tests of the Shuttle server and protocol: a simulated Mac (the same receive/send logic as
the Mac app, app/xfer.c) talks to the server through a proxy that can garble and drop bytes.

    python3 -I shuttle/server/test_server.py            (about 20 s)

Covers: listing, MacBinary of native files (both forks, type, creator), .bin pass-through, text
conversion, raw disk image -> Disk Copy 4.2, upload saved as a native file, cancel, the 32 KB hold
(stop acking, write, ack), and all of it again over a line that corrupts and drops bytes.
"""
import asyncio
import os
import random
import struct
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shuttle_server as S  # noqa: E402

S.log = lambda *a: None
S.SENDER_TIMEOUT = 2.0


class Lossy:
    """TCP proxy: bytes in both directions, each corrupted with probability p and dropped with q."""

    def __init__(self, target_port, p=0.0, q=0.0, seed=1):
        self.port, self.p, self.q = target_port, p, q
        self.rng = random.Random(seed)
        self.damaged = 0

    async def start(self):
        self.srv = await asyncio.start_server(self.conn, "127.0.0.1", 0)
        return self.srv.sockets[0].getsockname()[1]

    async def conn(self, r, w):
        r2, w2 = await asyncio.open_connection("127.0.0.1", self.port)

        async def pump(src, dst):
            try:
                while True:
                    d = await src.read(512)
                    if not d:
                        break
                    out = bytearray()
                    for b in d:
                        x = self.rng.random()
                        if x < self.q:
                            self.damaged += 1
                            continue
                        if x < self.q + self.p:
                            self.damaged += 1
                            b ^= 1 << self.rng.randrange(8)
                        out.append(b)
                    dst.write(bytes(out))
                    await dst.drain()
            except (ConnectionError, OSError):
                pass
            dst.close()
        await asyncio.gather(pump(r, w2), pump(r2, w))


class MacSim:
    def __init__(self, r, w, W=4, B=1024, bufsize=32768, bps=200000):
        self.r, self.w, self.W, self.B, self.bufsize = r, w, W, B, bufsize
        self.dec = S.Decoder()
        self.q = []
        self.nak_wait = W * B / bps + 0.3
        self.flushes = self.naks = 0

    async def get(self, timeout):
        while not self.q:
            try:
                d = await asyncio.wait_for(self.r.read(4096), timeout)
            except asyncio.TimeoutError:
                return None
            if not d:
                raise ConnectionError("closed")
            self.q += self.dec.feed(d)
        return self.q.pop(0)

    def line(self, s):
        self.w.write(s.encode("mac_roman") + b"\r")

    def frame(self, t, tid, n, payload=b""):
        self.w.write(S.encode_frame(t, tid, n & 0xFFFF, payload))

    async def greeting(self, timeout=10):
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout:
            it = await self.get(0.5)
            if it and it[0] == "line" and it[1].startswith(b"SHUTTLE"):
                return it[1].decode("mac_roman")
        raise TimeoutError("no greeting")

    async def cmd(self, s, tries=6):
        """A command in a C frame; waits for the R frame answering it, sending it again if needed."""
        self.cmdno = getattr(self, "cmdno", 0) % 65535 + 1
        for _ in range(tries):
            self.frame(0x43, 0, self.cmdno, s.encode("mac_roman"))
            t0 = time.monotonic()
            while time.monotonic() - t0 < 1.5:
                it = await self.get(0.2)
                if it and it[0] == "frame" and it[1] == 0x52 and it[3] == self.cmdno:
                    r = it[4].decode("mac_roman")
                    if r.startswith("NOTE"):
                        t0 = time.monotonic()
                        continue
                    if r.startswith(("OK", "ERR")):
                        return r
        raise TimeoutError("no reply to " + s)

    def is_text(self, it, prefix):
        return it and it[0] == "frame" and it[1] == 0x52 and it[4].startswith(prefix)

    async def receive(self, tid, size, cancel_after=None):
        W, B = self.W, self.B
        n = (size + B - 1) // B
        nxt, granted, used, holding = 0, W, 0, False
        out, disk = bytearray(), bytearray()
        last_nak, progress, tries = (None, 0.0), time.monotonic(), 0
        seen = nak_seen = -1

        def flush():
            nonlocal used
            disk.extend(out)
            out.clear()
            used = 0
            self.flushes += 1

        def nak(force=False):
            nonlocal holding, granted, last_nak, nak_seen
            if holding:
                flush()
                holding = False
            now = time.monotonic()
            if force or last_nak[0] != nxt or now - last_nak[1] > self.nak_wait:
                nak_seen = seen
                self.frame(0x4E, tid, nxt)
                self.naks += 1
                last_nak = (nxt, now)
                granted = max(granted, nxt + W)

        while True:
            if nxt == n:
                flush()
                for _ in range(6):
                    self.frame(0x41, tid, nxt)
                    t0 = time.monotonic()
                    while time.monotonic() - t0 < 1.0:
                        it = await self.get(0.2)
                        if self.is_text(it, b"DONE %d" % tid):
                            return bytes(disk)
                raise TimeoutError("no DONE")
            if cancel_after is not None and nxt >= cancel_after:
                return await self.cmd("CANCEL")
            it = await self.get(0.1)
            if it and it[0] == "frame" and it[1] == 0x44 and it[2] == tid:
                d = ((it[3] - nxt + 0x8000) & 0xFFFF) - 0x8000
                rewound = nxt + d <= nak_seen
                seen = max(seen, nxt + d)
                if d == 0 and len(it[4]) == min(B, size - nxt * B):
                    out += it[4]
                    used += len(it[4])
                    nxt += 1
                    progress, tries = time.monotonic(), 0
                    if nxt == n:
                        continue
                    if not holding and used + W * B <= self.bufsize:
                        self.frame(0x41, tid, nxt)
                        granted = max(granted, nxt + W)
                    else:
                        holding = True
                    if holding and nxt >= granted:
                        flush()
                        holding = False
                        self.frame(0x41, tid, nxt)
                        granted = nxt + W
                elif d < 0:
                    if not holding:
                        self.frame(0x41, tid, nxt)
                else:
                    nak(force=rewound)
            elif it and it[0] == "bad":
                nak()
            if time.monotonic() - progress > self.nak_wait + 0.5:
                tries += 1
                if tries > 10:
                    raise TimeoutError("receive stalled")
                progress = time.monotonic()
                nak(force=True)

    async def send(self, tid, data, chunk=32768):
        W, B = self.W, self.B
        n = (len(data) + B - 1) // B
        base = nxt = 0
        cend = min(n, chunk // B)
        progress, tries = time.monotonic(), 0
        while True:
            if base == n:
                for _ in range(6):
                    t0 = time.monotonic()
                    while time.monotonic() - t0 < 1.5:
                        it = await self.get(0.2)
                        if self.is_text(it, b"OK SAVED") or self.is_text(it, b"ERR"):
                            return it[4].decode("mac_roman")
                    k = n - 1
                    self.frame(0x44, tid, k, data[k * B:(k + 1) * B])
                raise TimeoutError("no OK SAVED")
            if base == cend and cend < n:
                cend = min(n, cend + chunk // B)
            while nxt < min(base + W, cend):
                self.frame(0x44, tid, nxt, data[nxt * B:(nxt + 1) * B])
                nxt += 1
            it = await self.get(0.1)
            if it and it[0] == "frame" and it[1] in (0x41, 0x4E) and it[2] == tid:
                dd = (it[3] - base) & 0xFFFF
                if dd <= nxt - base:
                    full = base + dd
                    if full > base or it[1] == 0x4E:
                        progress, tries = time.monotonic(), 0
                    base = max(base, full)
                    if it[1] == 0x4E:
                        nxt = full
            if time.monotonic() - progress > 2.0:
                tries += 1
                if tries > 8:
                    raise TimeoutError("send stalled")
                progress = time.monotonic()
                nxt = base


def make_tree(root):
    os.makedirs(os.path.join(root, "Games"))
    rng = random.Random(7)
    data = bytes(rng.randrange(256) for _ in range(150000))
    rsrc = bytes(rng.randrange(256) for _ in range(160123))
    p = os.path.join(root, "Games", "Big Test")
    open(p, "wb").write(data)
    open(S.rsrc_path(p), "wb").write(rsrc)
    S.set_finder_info(p, b"TSTFShtl" + b"\x21\x00" + bytes(22))
    open(os.path.join(root, "notes.txt"), "w", encoding="utf-8").write("café “quoted”\nline two\n")
    open(os.path.join(root, "disk.dsk"), "wb").write(bytes(range(256)) * 3200)
    mb = S.macbinary(b"Packed App", b"APPL", b"PkAp", 0x2000, b"", b"RSRC" * 300)
    open(os.path.join(root, "Packed App.bin"), "wb").write(mb)
    open(os.path.join(root, ".hidden"), "w").write("x")
    return data, rsrc


class T(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="shuttle_test_")
        cls.root = os.path.join(cls.tmp, "root")
        cls.data, cls.rsrc = make_tree(cls.root)

    def run_with(self, p=0.0, q=0.0, body=None, bufsize=32768):
        async def go():
            srv = S.Server(self.root, os.path.join(self.tmp, "cache"), "TestBook", 600)

            async def on_conn(r, w):
                await S.Session(srv, r, w).run()
            server = await asyncio.start_server(on_conn, "127.0.0.1", 0)
            port = server.sockets[0].getsockname()[1]
            lossy = Lossy(port, p, q)
            lp = await lossy.start()
            r, w = await asyncio.open_connection("127.0.0.1", lp)
            mac = MacSim(r, w, bufsize=bufsize)
            greet = await mac.greeting()
            self.assertTrue(greet.startswith("SHUTTLE 1 TestBook"), greet)
            self.assertTrue((await mac.cmd("HELLO 1 4 1024 200000")).startswith("OK HELLO 1 4 1024"))
            try:
                await body(mac)
            finally:
                w.close()
                server.close()
                lossy.srv.close()
            return lossy.damaged
        return asyncio.run(asyncio.wait_for(go(), 120))

    async def listing(self, mac, path):
        r = await mac.cmd("LIST " + path)
        self.assertTrue(r.startswith("OK LIST"), r)
        _, _, tid, size, count = r.split()
        text = await mac.receive(int(tid), int(size))
        lines = [ln for ln in text.split(b"\r") if ln]
        self.assertEqual(len(lines), int(count))
        return lines

    async def get(self, mac, path):
        r = await mac.cmd("GET " + path)
        self.assertTrue(r.startswith("OK GET"), r)
        _, _, tid, size, dl, rl = r.split()
        mb = await mac.receive(int(tid), int(size))
        self.assertEqual(len(mb), int(size))
        m = S.parse_macbinary(mb)
        self.assertIsNotNone(m)
        self.assertEqual((len(m[4]), len(m[5])), (int(dl), int(rl)))
        return m

    async def everything(self, mac):
        top = await self.listing(mac, ":")
        names = [ln.split(b" ", 1)[0] + b" " + ln.rsplit(b" ", 1)[-1] for ln in top]
        self.assertIn(b"D 1 Games", top)
        self.assertTrue(any(ln.startswith(b"F 23 0 54455854 74747874 notes.txt") for ln in top), top)
        self.assertTrue(any(ln.endswith(b"Packed App") and b"4150504C" in ln for ln in top), top)
        self.assertFalse(any(b"hidden" in ln for ln in top), names)
        games = await self.listing(mac, "Games")
        self.assertEqual(games, [b"F 150000 160123 54535446 5368746C Big Test"])
        m = await self.get(mac, "Games:Big Test")
        self.assertEqual(m[0], b"Big Test")
        self.assertEqual((m[1], m[2], m[3]), (b"TSTF", b"Shtl", 0x2100))
        self.assertEqual(m[4], self.data)
        self.assertEqual(m[5], self.rsrc)
        t = await self.get(mac, "notes.txt")
        self.assertEqual(t[4], b"caf\x8e \xd2quoted\xd3\rline two\r")
        self.assertEqual((t[1], t[2]), (b"TEXT", b"ttxt"))
        d = await self.get(mac, "disk.dsk")
        self.assertEqual((d[1], d[2]), (b"dImg", b"dCpy"))
        self.assertEqual(len(d[4]), 84 + 819200 + 19200)
        self.assertEqual(d[4][0x50:0x54], b"\x01\x22\x01\x00")
        b = await self.get(mac, "Packed App")
        self.assertEqual((b[1], b[5]), (b"APPL", b"RSRC" * 300))
        # upload
        up = S.macbinary(b"From the Plus", b"APPL", b"Shtl", 0x2100, b"hello" * 99, b"R" * 40000)
        r = await mac.cmd("PUT From the Plus %d" % len(up))
        self.assertTrue(r.startswith("OK PUT"), r)
        r = await mac.send(int(r.split()[2]), up)
        self.assertTrue(r.startswith("OK SAVED"), r)
        saved = os.path.join(self.root, "From Plus", r.split(" ", 3)[3])
        self.assertEqual(open(saved, "rb").read(), b"hello" * 99)
        self.assertEqual(S.read_rsrc(saved), b"R" * 40000)
        self.assertEqual(S.get_finder_info(saved)[:8], b"APPLShtl")
        # cancel
        r = await mac.cmd("GET Games:Big Test")
        tid = int(r.split()[2])
        r = await mac.receive(tid, int(r.split()[3]), cancel_after=20)
        self.assertEqual(r, "OK CANCEL")
        self.assertTrue((await mac.cmd("PING")).startswith("OK PING"))
        top = await self.listing(mac, ":")
        self.assertTrue(any(ln.endswith(b"D 1 From Plus") or b"From Plus" in ln for ln in top))
        r = await mac.cmd("GET nothing here")
        self.assertTrue(r.startswith("ERR NOTFOUND"), r)
        self.assertEqual(await mac.cmd("BYE"), "OK BYE")

    def test_clean_line(self):
        self.run_with(body=self.everything)

    def test_lossy_line(self):
        damaged = self.run_with(p=0.0001, q=0.00005, body=self.everything)
        self.assertGreater(damaged, 5)

    def test_small_buffer_holds(self):
        async def body(mac):
            await self.get(mac, "Games:Big Test")
            self.assertGreater(mac.flushes, 30)
        self.run_with(body=body, bufsize=8192)

    def test_archive(self):
        """A StuffIt archive (the Disk Copy 4.2 download, if build/vendor has it) is a folder of what's inside."""
        sit = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "vendor",
                           "Disk_Copy_(v4.2).sit")
        if not os.path.exists(sit) or not S.shutil.which("unar"):
            self.skipTest("no archive or no unar")
        os.makedirs(os.path.join(self.root, "Archives"), exist_ok=True)
        with open(sit, "rb") as f, open(os.path.join(self.root, "Archives", "Disk_Copy_(v4.2).sit"), "wb") as g:
            g.write(f.read())

        async def body(mac):
            top = await self.listing(mac, "Archives")
            self.assertEqual(top, [b"A 15672 Disk_Copy_(v4.2).sit"])
            inner = await self.listing(mac, "Archives:Disk_Copy_(v4.2).sit")
            self.assertEqual(inner, [b"D 2 Disk Copy (v4.2)"])
            files = await self.listing(mac, "Archives:Disk_Copy_(v4.2).sit:Disk Copy (v4.2)")
            self.assertIn(b"F 0 24359 4150504C 64437079 Disk Copy 4.2", files)
            m = await self.get(mac, "Archives:Disk_Copy_(v4.2).sit:Disk Copy (v4.2):Disk Copy 4.2")
            self.assertEqual((m[1], m[2], len(m[4]), len(m[5])), (b"APPL", b"dCpy", 0, 24359))
            self.assertIn(b"4.2, Copyright", m[5])
        self.run_with(body=body)

    def test_dc42_checksum(self):
        img = S.dc42(bytes(range(256)) * 3200, b"Test")
        def ck(b):
            s = 0
            for i in range(0, len(b), 2):
                s = (s + (b[i] << 8 | b[i + 1])) & 0xFFFFFFFF
                s = ((s >> 1) | (s << 31)) & 0xFFFFFFFF
            return s
        self.assertEqual(struct.unpack(">I", img[0x48:0x4C])[0], ck(img[84:84 + 819200]))

    def test_names(self):
        self.assertEqual(S.mac_name("a:b"), b"a/b")
        self.assertEqual(len(S.mac_name("x" * 40 + ".txt")), 31)
        self.assertTrue(S.mac_name("x" * 40 + ".txt").endswith(b"\xc9.txt"))
        self.assertEqual(S.posix_name(b"a/b"), "a:b")


if __name__ == "__main__":
    unittest.main(verbosity=2)

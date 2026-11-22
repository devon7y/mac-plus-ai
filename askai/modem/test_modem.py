#!/usr/bin/env python3
"""Test the Hayes modem emulator with the relay (mock provider): AT commands, dialling, a chat turn,
+++ escape, ATO, ATH, a failed dial and the relay hanging up. About 5 s, no network beyond 127.0.0.1.

    python3 askai/modem/test_modem.py
"""
import asyncio
import os
import sys
import unittest
from types import SimpleNamespace

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "relay"))
import hayes_emu  # noqa: E402
import relay  # noqa: E402


class Line:
    """The Mac's end of the serial line."""

    def __init__(self, r, w):
        self.r, self.w, self.buf = r, w, b""

    async def send(self, s):
        self.w.write(s.encode("mac_roman") if isinstance(s, str) else s)
        await self.w.drain()

    async def expect(self, token, timeout=5):
        tok = token.encode()
        while tok not in self.buf:
            data = await asyncio.wait_for(self.r.read(4096), timeout)
            if not data:
                raise EOFError(self.buf)
            self.buf += data
        k = self.buf.index(tok) + len(tok)
        got, self.buf = self.buf[:k], self.buf[k:]
        return got


class TestModem(unittest.TestCase):
    def test_chain(self):
        async def main():
            raw = {"relay": {"name": "Test relay", "pin": "77", "default_model": "mock"},
                   "provider": {"mock": {"type": "mock", "latency": 0.05, "chars_per_second": 4000}},
                   "model": [{"id": "mock", "label": "Mock", "tag": "Mock", "provider": "mock", "model": "m"}]}
            rl = relay.Relay(relay.Config(raw))
            rsrv = await asyncio.start_server(rl.handle, "127.0.0.1", 0)
            rport = rsrv.sockets[0].getsockname()[1]
            args = SimpleNamespace(guard=0.3, connect_speed=19200, baud=0, latency=0.0, connect_timeout=3,
                                   phonebook={"5551234": "127.0.0.1:%d" % rport}, quiet=True,
                                   log_data=False)

            async def on_serial(r, w):
                await hayes_emu.Modem(args).serial(r, w)
            msrv = await asyncio.start_server(on_serial, "127.0.0.1", 0)
            r, w = await asyncio.open_connection("127.0.0.1", msrv.sockets[0].getsockname()[1])
            ln = Line(r, w)
            await ln.send("AT\r")
            self.assertEqual(await ln.expect("OK\r\n"), b"AT\r\r\nOK\r\n")              # echo + result
            await ln.send("ATE0 V1 S0=0 &C1 &D2 +CONFIG\r")       # an init string with vendor parts
            self.assertEqual(await ln.expect("OK\r\n"), b"ATE0 V1 S0=0 &C1 &D2 +CONFIG\r\r\nOK\r\n")
            await ln.send("ATQ0X4\r")
            self.assertEqual(await ln.expect("OK\r\n"), b"\r\nOK\r\n")                # no echo now
            await ln.send("ATFOO\r")
            self.assertEqual(await ln.expect("ERROR\r\n"), b"\r\nERROR\r\n")
            # a dial that fails
            await ln.send("ATDT 127.0.0.1:1\r")
            self.assertIn(b"NO CARRIER", await ln.expect("NO CARRIER\r\n"))
            # dial the relay
            await ln.send("ATDT127.0.0.1:%d\r" % rport)
            self.assertIn(b"CONNECT 19200", await ln.expect("CONNECT 19200\r\n"))
            self.assertIn(b"ASKAI 1 Test relay\r", await ln.expect("\r", 5))
            await ln.send("HELLO 1 77\r")
            self.assertIn(b"OK HELLO", await ln.expect("\r"))
            await ln.send("ASK hi there\r")
            got = await ln.expect("END done")
            self.assertIn(b"BEGIN mock Mock\r", got)
            self.assertIn(b'You said: "hi there"', got.replace(b"\rT ", b""))
            await ln.expect("\r")
            # +++ with guard times: command mode, the connection is held
            await asyncio.sleep(0.4)
            await ln.send("+++")
            await asyncio.sleep(0.45)
            self.assertIn(b"OK", await ln.expect("OK\r\n"))
            await ln.send("AT\r")
            await ln.expect("OK\r\n")
            await ln.send("ATO\r")
            await ln.expect("CONNECT 19200\r\n")
            await ln.send("PING\r")
            self.assertEqual(await ln.expect("\r"), b"PONG\r")
            # "+++" inside data (no guard time) goes through to the relay
            await ln.send("ASK a+++b\r")
            got = await ln.expect("END done")
            self.assertIn(b'You said: "a+++b"', got.replace(b"\rT ", b""))
            await ln.expect("\r")
            # escape and hang up
            await asyncio.sleep(0.4)
            await ln.send("+++")
            await asyncio.sleep(0.45)
            await ln.expect("OK\r\n")
            await ln.send("ATH\r")
            await ln.expect("OK\r\n")
            await ln.send("ATO\r")
            self.assertIn(b"ERROR", await ln.expect("ERROR\r\n"))
            # dial again by phone-book number; BYE makes the relay hang up: NO CARRIER
            await ln.send("ATDT 555-1234\r")
            await ln.expect("CONNECT 19200\r\n")
            await ln.expect("\r")
            await ln.send("HELLO 1 77\rBYE\r")
            await ln.expect("OK BYE\r")
            self.assertIn(b"NO CARRIER", await ln.expect("NO CARRIER\r\n"))
            # numeric result codes
            await ln.send("ATV0\r")
            self.assertEqual(await ln.expect("0\r"), b"0\r")
            w.close()
            msrv.close()
            rsrv.close()
        stderr, sys.stderr = sys.stderr, open(os.devnull, "w")
        try:
            asyncio.run(main())
        finally:
            sys.stderr.close()
            sys.stderr = stderr


if __name__ == "__main__":
    unittest.main(verbosity=1)

#!/usr/bin/env python3
"""Hayes-compatible WiFi modem emulator, for testing Shuttle without hardware. A copy of
askai/modem/hayes_emu.py (read-only for Shuttle) with what the Plus Modem firmware (modem/) adds:
dial aliases (AT+ALIAS name=host:port, --alias) and the boot banner (--banner: "PLUS MODEM READY"
every 2 s until the Mac sends its first byte, at most 60 s).

    python3 hayes_emu.py [--listen 127.0.0.1:2323] [--alias mac=127.0.0.1:6801] [--banner] [--guard 1.0]

The "serial line" is a TCP socket: MAME's Mac Plus modem port connects to it with
    mame macplus ... -modem null_modem -bitb socket.127.0.0.1:2323
(MAME is the client), or a test script does. On the line it behaves like the real thing:

- command mode: AT commands, echo (E1, as shipped), verbose or numeric result codes (V1/V0), quiet (Q1),
  S registers (S2 escape character, S12 guard time), ATZ / AT&F, ATI, ATH, ATO, AT&W (accepted),
  AT&C/&D/&K and speaker settings (accepted), and the vendor extensions AT+..., AT*..., AT$... and
  ATB<baud> (accepted with OK, so init strings for any of the three families work);
- ATDT host:port (or ATD"host:port", ATDP, ATD host port, a phone-book number) opens a TCP connection:
  CONNECT <speed>, then data mode; a refused or timed-out connection gives NO CARRIER;
- in data mode bytes go straight through; the far end closing gives NO CARRIER and command mode;
- "+++" with a guard time of silence before and after (S12, default 1 s) returns to command mode with
  the connection held: ATO goes back online, ATH hangs up.
"""
import argparse
import asyncio
import sys
import time

RESULTS = {0: "OK", 1: "CONNECT", 2: "RING", 3: "NO CARRIER", 4: "ERROR", 6: "NO DIALTONE", 7: "BUSY",
           8: "NO ANSWER"}


class Modem:
    def __init__(self, a):
        self.a = a
        self.reset()
        self.tcp_r = self.tcp_w = None
        self.online = False
        self.pump = None
        self.last_cmd = ""

    def reset(self):
        self.echo, self.verbose, self.quiet = True, True, False
        self.S = {0: 0, 2: 43, 3: 13, 4: 10, 5: 8, 12: int(round(self.a.guard * 50))}

    def log(self, *x):
        if not self.a.quiet:
            print(time.strftime("%H:%M:%S"), "modem:", *x, file=sys.stderr, flush=True)

    @property
    def guard(self):
        return self.S[12] / 50.0

    # ------------------------------------------------------------------------------- serial side
    async def banner(self):
        # (a short wait first: MAME's null modem stalls for good if a byte arrives before the Lua script's
        # baud-rate change has taken effect, which happens on its first frames)
        await asyncio.sleep(0.5)
        t0 = time.monotonic()
        while not self.heard and time.monotonic() - t0 < 60:
            self.out(b"PLUS MODEM READY\r\n")
            await asyncio.sleep(2.0)

    async def serial(self, reader, writer):
        self.ser = writer
        self.cmd = bytearray()
        self.plus = 0
        self.plus_timer = None
        self.last_rx = 0.0
        self.heard = False
        self.log("serial line connected", writer.get_extra_info("peername"))
        if self.a.banner:
            asyncio.ensure_future(self.banner())
        try:
            while True:
                data = await reader.read(1024)
                if not data:
                    break
                self.heard = True
                if self.a.log_data:
                    self.log("serial in:", data.hex(" "), repr(data))
                for b in data:
                    await self.byte(b)
        except (ConnectionError, OSError):
            pass
        finally:
            self.log("serial line closed")
            await self.hangup()

    def out(self, data):
        try:
            self.ser.write(data)
        except (ConnectionError, OSError, AttributeError):
            pass

    def result(self, code, extra=""):
        if self.quiet:
            return
        if self.verbose:
            self.out(b"\r\n" + (RESULTS[code] + extra).encode() + b"\r\n")
        else:
            self.out(b"%d\r" % code)
        self.log("->", RESULTS[code] + extra)

    async def byte(self, b):
        now = time.monotonic()
        if self.online:
            await self.data_byte(b, now)
        else:
            self.command_byte(b)
        self.last_rx = now

    async def data_byte(self, b, now):
        esc = self.S[2]
        if b == esc and self.plus < 3 and (self.plus > 0 or now - self.last_rx >= self.guard):
            self.plus += 1                                  # hold it back: maybe an escape
            if self.plus == 3:
                self.plus_timer = asyncio.get_running_loop().call_later(self.guard, self.escape)
            return
        if self.plus:                                       # not an escape after all
            if self.plus_timer:
                self.plus_timer.cancel()
                self.plus_timer = None
            await self.to_tcp(bytes([esc]) * self.plus)
            self.plus = 0
        await self.to_tcp(bytes([b]))

    def escape(self):
        self.plus_timer = None
        if self.plus == 3 and self.online:
            self.plus = 0
            self.online = False                             # the connection stays up
            self.log("escape (+++): command mode, connection held")
            self.result(0)

    async def to_tcp(self, data):
        if self.tcp_w:
            try:
                self.tcp_w.write(data)
                await self.tcp_w.drain()
            except (ConnectionError, OSError):
                pass

    def command_byte(self, b):
        if b in (8, 127):
            if self.cmd:
                self.cmd.pop()
                if self.echo:
                    self.out(b"\x08 \x08")
            return
        if self.echo:
            self.out(bytes([b]))
        if b == 10:
            return
        if b == self.S[3]:
            line = self.cmd.decode("latin-1")
            self.cmd = bytearray()
            asyncio.ensure_future(self.execute(line.strip()))
            return
        if len(self.cmd) < 255:
            self.cmd.append(b)
        if self.cmd[-2:].upper() == b"A/":                 # A/ repeats the last command, no CR needed
            self.cmd = bytearray()
            if self.echo:
                self.out(b"\r")
            asyncio.ensure_future(self.execute(self.last_cmd))

    # ------------------------------------------------------------------------------- AT commands
    async def execute(self, line):
        if not line[:2].upper() == "AT":
            return                                          # not a command: ignored, as real modems do
        self.last_cmd = line
        self.log("<-", line)
        s = line[2:]
        if s.upper().startswith("+ALIAS"):                  # AT+ALIAS name=host:port (as the Plus Modem firmware)
            arg = s[6:].strip().lstrip("=").strip()
            if "=" in arg:
                k, _, v = arg.partition("=")
                self.a.aliases[k.strip().lower()] = v.strip()
            else:
                for k, v in sorted(self.a.aliases.items()):
                    self.out(b"\r\n%s=%s" % (k.encode(), v.encode()))
            self.result(0)
            return
        i = 0

        def num():
            nonlocal i
            j = i
            while i < len(s) and s[i].isdigit():
                i += 1
            return int(s[j:i]) if i > j else 0
        while i < len(s):
            c = s[i].upper()
            i += 1
            if c == " ":
                continue
            if c == "D":
                await self.dial(s[i:])
                return
            if c in "+*$#":                                 # vendor extensions (AT+CONFIG, AT*B, ...)
                break
            if c == "E":
                self.echo = num() == 1
            elif c == "V":
                self.verbose = num() == 1
            elif c == "Q":
                self.quiet = num() == 1
            elif c == "Z":
                num()
                await self.hangup()
                self.reset()
            elif c == "H":
                num()
                await self.hangup()
            elif c == "O":
                num()
                if self.tcp_w:
                    self.online = True
                    self.result(1, " %d" % self.a.connect_speed)
                else:
                    self.result(4)
                return
            elif c == "I":
                n = num()
                self.out(b"\r\nShuttle test modem 1.0 (ATI%d)\r\n" % n)
            elif c in "XMLBNWCY":
                num()                                       # result set, speaker, baud (Zimodem ATB): accepted
            elif c == "&":
                if i < len(s):
                    c2 = s[i].upper()
                    i += 1
                    num()
                    if c2 == "F":
                        self.reset()
            elif c == "S":
                r = num()
                if i < len(s) and s[i] == "=":
                    i += 1
                    self.S[r] = num()
                elif i < len(s) and s[i] == "?":
                    i += 1
                    self.out(b"\r\n%03d\r\n" % self.S.get(r, 0))
            else:
                self.result(4)
                return
        self.result(0)

    async def dial(self, rest):
        d = rest.strip()
        if d[:1].upper() in ("T", "P"):
            d = d[1:]
        d = d.strip().strip('"').strip()
        if d.endswith(";"):
            d = d[:-1]
        if not d:
            self.result(3)
            return
        target = self.a.phonebook.get(d.replace("-", "").replace(" ", ""), d)
        target = self.a.aliases.get(target.strip().lower(), target)
        if ":" in target:
            host, _, port = target.rpartition(":")
        elif " " in target:
            host, _, port = target.rpartition(" ")
        elif "/" in target:
            host, _, port = target.rpartition("/")
        else:
            host, port = target, "23"
        if self.tcp_w:
            await self.hangup()
        try:
            self.log("dialling", host, port)
            self.tcp_r, self.tcp_w = await asyncio.wait_for(
                asyncio.open_connection(host.strip(), int(port)), self.a.connect_timeout)
        except (OSError, ValueError, asyncio.TimeoutError) as e:
            self.log("connect failed:", type(e).__name__, e)
            self.tcp_r = self.tcp_w = None
            await asyncio.sleep(0.5)
            self.result(3)
            return
        self.online = True
        self.plus = 0
        self.result(1, " %d" % self.a.connect_speed)
        self.pump = asyncio.ensure_future(self.from_tcp(self.tcp_r, self.tcp_w))

    async def from_tcp(self, reader, writer):
        per_byte = 10.0 / self.a.baud if self.a.baud else 0.0
        try:
            while True:
                data = await reader.read(512)
                if not data:
                    break
                if self.a.latency:
                    await asyncio.sleep(self.a.latency)
                if self.online:                             # in command mode the far end's bytes are dropped
                    if per_byte:
                        for k in range(0, len(data), 16):
                            self.out(data[k:k + 16])
                            await asyncio.sleep(per_byte * len(data[k:k + 16]))
                    else:
                        self.out(data)
        except (ConnectionError, OSError):
            pass
        if self.tcp_w is writer:                            # the far end hung up
            self.tcp_r = self.tcp_w = None
            self.log("remote closed")
            self.online = False
            self.result(3)

    async def hangup(self):
        w = self.tcp_w
        self.tcp_r = self.tcp_w = None
        self.online = False
        if self.pump:
            self.pump.cancel()
            self.pump = None
        if w:
            self.log("hang up")
            w.close()


def main():
    ap = argparse.ArgumentParser(description="Hayes WiFi modem emulator (serial side is a TCP socket)")
    ap.add_argument("--listen", default="127.0.0.1:2323", help="serial-side address MAME connects to")
    ap.add_argument("--guard", type=float, default=1.0, help="+++ guard time in seconds (S12/50)")
    ap.add_argument("--connect-speed", type=int, default=19200, help="speed in the CONNECT message")
    ap.add_argument("--baud", type=int, default=0, help="pace far-end bytes to this baud (0: MAME does it)")
    ap.add_argument("--latency", type=float, default=0.0, help="extra delay for far-end data (seconds)")
    ap.add_argument("--connect-timeout", type=float, default=10.0)
    ap.add_argument("--phonebook", action="append", default=[], help="NUMBER=host:port (repeatable)")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--log-data", action="store_true", help="log every byte from the serial line")
    ap.add_argument("--alias", action="append", default=[], help="NAME=host:port (repeatable), as AT+ALIAS")
    ap.add_argument("--banner", action="store_true", help="send PLUS MODEM READY until the Mac speaks")
    a = ap.parse_args()
    a.phonebook = dict(p.split("=", 1) for p in a.phonebook)
    a.aliases = {k.strip().lower(): v.strip() for k, v in (p.split("=", 1) for p in a.alias)}
    host, _, port = a.listen.rpartition(":")

    async def run():
        async def on_serial(reader, writer):
            await Modem(a).serial(reader, writer)
        srv = await asyncio.start_server(on_serial, host or "127.0.0.1", int(port))
        print("modem emulator: serial line on %s (connect MAME with -bitb socket.%s)" % (a.listen, a.listen),
              file=sys.stderr, flush=True)
        async with srv:
            await srv.serve_forever()
    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

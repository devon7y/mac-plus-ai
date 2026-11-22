#!/usr/bin/env python3
"""Ask AI relay: plain-text chat over TCP for a 1986 Macintosh, HTTPS to modern model APIs.

    python3 relay.py [--config relay.toml] [--host 0.0.0.0] [--port 6800] [--pin 1234]
    python3 relay.py --check          show the models and which API-key variables are set, then exit
    python3 relay.py --mock-only      offer only the mock provider (testing; no network, no keys)

The Mac (through its modem port, a Hayes WiFi modem and TCP) speaks the line protocol in
PROTOCOL.md. The relay keeps each session's conversation, turns the model's Markdown into plain
Mac Roman text and streams it back. API keys come only from environment variables named in the
config; the Mac never sees them. Standard library only (Python 3.8 or later).
"""
import argparse
import asyncio
import collections
import hmac
import os
import secrets
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import providers  # noqa: E402
import textconv  # noqa: E402

PROTO_VERSION = 1
FLUSH_S = 0.12               # reply text is sent at least this often (fewer, longer T lines)
DEFAULT_SYSTEM = ("You are chatting with someone who types on a 1986 Apple Macintosh Plus over a "
                  "serial line. Its screen is 512 by 342 pixels, black and white. Answer in plain "
                  "text with short paragraphs: no tables, no emoji, no wide ASCII art. Keep answers "
                  "brief (under 150 words) unless asked for more.")
DEFAULTS = {
    "name": "Ask AI relay",
    "host": "0.0.0.0",
    "port": 6800,
    "pin": "",
    "max_connections": 4,
    "idle_minutes": 30,
    "hello_seconds": 120,
    "max_line_bytes": 2048,
    "max_turn_chars": 2000,
    "max_reply_tokens": 1024,
    "max_history_chars": 24000,
    "replies_per_minute": 8,
    "replies_per_day": 400,
    "pin_tries": 3,
    "lockout_failures": 10,
    "lockout_minutes": 60,
    "resume_minutes": 60,
    "system_prompt": DEFAULT_SYSTEM,
    "default_model": "",
    "log_text": False,
}

MOCK_CONFIG = {
    "relay": {"name": "Ask AI relay (mock)", "default_model": "mock"},
    "provider": {"mock": {"type": "mock"}},
    "model": [{"id": "mock", "label": "Mock Model (test)", "tag": "Mock", "provider": "mock", "model": "mock-1"},
              {"id": "mock-fast", "label": "Mock Model, fast", "tag": "Fast", "provider": "mock",
               "model": "mock-fast"}],
}


def log(*a):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), *a, file=sys.stderr, flush=True)


def load_config(path):
    try:
        import tomllib
    except ImportError:                                 # Python < 3.11
        tomllib = None
    if path is None:
        for p in (os.environ.get("ASKAI_CONFIG"), os.path.expanduser("~/.askai/relay.toml"),
                  os.path.join(HERE, "relay.toml"), os.path.join(HERE, "relay.example.toml")):
            if p and os.path.exists(p):
                path = p
                break
    with open(path, "rb") as f:
        data = f.read()
    if tomllib:
        cfg = tomllib.loads(data.decode("utf-8"))
    else:
        import minitoml
        cfg = minitoml.loads(data.decode("utf-8"))
    cfg["_path"] = path
    return cfg


class Config:
    def __init__(self, raw, mock_only=False):
        if mock_only:
            raw = dict(MOCK_CONFIG, _path="(built-in mock config)")
        r = dict(DEFAULTS)
        r.update(raw.get("relay", {}))
        self.r = r
        self.path = raw.get("_path", "")
        self.providers = {}
        for name, pc in (raw.get("provider") or {}).items():
            self.providers[name] = providers.make(name, pc)
        self.models = []                                # dicts: id, label, tag, provider, model, ...
        seen = set()
        for m in raw.get("model") or []:
            mid = str(m.get("id", "")).strip()
            if not mid or " " in mid or len(mid) > 31 or mid in seen:
                raise ValueError("model ids must be unique, 1-31 characters, without blanks: %r" % mid)
            if m.get("provider") not in self.providers:
                raise ValueError("model %s: unknown provider %r" % (mid, m.get("provider")))
            seen.add(mid)
            m = dict(m)
            m["label"] = str(m.get("label", mid))[:40]
            m["tag"] = "".join(str(m.get("tag", "AI")).split())[:8] or "AI"
            self.models.append(m)
        if not self.models:
            raise ValueError("the config lists no models")

    def usable_models(self):
        out = []
        for m in self.models:
            ok, _ = providers.available(self.providers[m["provider"]].cfg)
            if ok:
                out.append(m)
        return out

    def model(self, mid):
        for m in self.usable_models():
            if m["id"] == mid:
                return m
        return None

    def default_model(self):
        ms = self.usable_models()
        if not ms:
            return None
        return self.model(self.r.get("default_model")) or ms[0]


class SessionState:
    """What survives a dropped line (resumable for resume_minutes with the same PIN)."""

    def __init__(self, sid, model):
        self.sid = sid
        self.history = []                # [{"role", "content"}]
        self.model = model
        self.system = None               # None: the relay's default
        self.recent = collections.deque()
        self.last_seen = time.time()
        self.attached = False


class Relay:
    def __init__(self, cfg):
        self.cfg = cfg
        self.conns = 0
        self.sessions = {}
        self.failures = collections.defaultdict(collections.deque)   # ip -> times of wrong PINs
        self.day = (time.strftime("%Y-%m-%d"), 0)

    # ---------------------------------------------------------------------------------- limits
    def locked(self, ip):
        q = self.failures[ip]
        now = time.time()
        while q and now - q[0] > self.cfg.r["lockout_minutes"] * 60:
            q.popleft()
        return len(q) >= self.cfg.r["lockout_failures"]

    def count_reply(self, st):
        now = time.time()
        while st.recent and now - st.recent[0] > 60:
            st.recent.popleft()
        if len(st.recent) >= self.cfg.r["replies_per_minute"]:
            return "Slow down: at most %d replies a minute." % self.cfg.r["replies_per_minute"]
        today = time.strftime("%Y-%m-%d")
        if self.day[0] != today:
            self.day = (today, 0)
        if self.day[1] >= self.cfg.r["replies_per_day"]:
            return "The relay's daily limit of %d replies is used up." % self.cfg.r["replies_per_day"]
        st.recent.append(now)
        self.day = (today, self.day[1] + 1)
        return None

    def purge_sessions(self):
        now = time.time()
        for sid in [s for s, st in self.sessions.items()
                    if not st.attached and now - st.last_seen > self.cfg.r["resume_minutes"] * 60]:
            del self.sessions[sid]

    # ------------------------------------------------------------------------------ connections
    async def handle(self, reader, writer):
        peer = writer.get_extra_info("peername") or ("?", 0)
        ip = peer[0]
        if self.locked(ip):
            log("refused", ip, "(locked out after wrong PINs)")
            writer.write(b"BYE LOCKED Too many wrong PINs; try again later.\r")
            await self._close(writer)
            return
        if self.conns >= self.cfg.r["max_connections"]:
            log("refused", ip, "(busy)")
            writer.write(b"BYE BUSY The relay is busy; try again later.\r")
            await self._close(writer)
            return
        self.conns += 1
        log("connect", "%s:%s" % peer[:2])
        c = Conn(self, reader, writer, ip)
        try:
            await c.run()
        except (ConnectionError, asyncio.IncompleteReadError, OSError) as e:
            log("connection error", ip, type(e).__name__)
        finally:
            self.conns -= 1
            await c.cleanup()
            await self._close(writer)
            log("disconnect", ip, "session", c.st.sid if c.st else "-")

    @staticmethod
    async def _close(writer):
        try:
            await writer.drain()
        except (ConnectionError, OSError):
            pass
        writer.close()
        try:
            await writer.wait_closed()
        except (ConnectionError, OSError):
            pass


class Conn:
    def __init__(self, relay, reader, writer, ip):
        self.relay = relay
        self.cfg = relay.cfg
        self.reader = reader
        self.writer = writer
        self.ip = ip
        self.st = None                   # SessionState after HELLO
        self.wrong = 0
        self.task = None                 # the reply being streamed
        self.cancel = None
        self.more = []                   # MORE lines waiting for ASK
        self.closing = False

    async def send(self, line):
        if isinstance(line, str):
            line = textconv.to_mac(line)
        self.writer.write(line + b"\r")
        await self.writer.drain()

    async def lines(self):
        """Lines from the Mac (CR or LF ends a line; empty lines are skipped)."""
        buf = b""
        overflow = False
        maxb = self.cfg.r["max_line_bytes"]
        timeout = self.cfg.r["hello_seconds"]
        while True:
            try:
                data = await asyncio.wait_for(self.reader.read(4096), timeout)
            except asyncio.TimeoutError:
                yield None
                return
            if not data:
                return
            timeout = self.cfg.r["idle_minutes"] * 60 if self.st else self.cfg.r["hello_seconds"]
            buf += data
            while True:
                i = min([k for k in (buf.find(b"\r"), buf.find(b"\n")) if k >= 0], default=-1)
                if i < 0:
                    break
                line, buf = buf[:i], buf[i + 1:]
                if overflow:
                    overflow = False
                    yield b"\x00TOOLONG"
                elif line.strip():
                    yield line
            if len(buf) > maxb:
                buf = b""
                overflow = True

    async def run(self):
        await self.send("ASKAI %d %s" % (PROTO_VERSION, self.cfg.r["name"]))
        async for raw in self.lines():
            if raw is None:
                await self.send("BYE IDLE " + ("No HELLO received." if not self.st else "Idle for too long."))
                return
            if raw == b"\x00TOOLONG":
                await self.send("ERR TOOLONG That line was too long.")
                continue
            line = textconv.from_mac(raw).strip("\n")
            cmd, _, arg = line.strip(" ").partition(" ")
            cmd = cmd.upper()
            if not await self.command(cmd, arg):
                return

    async def command(self, cmd, arg):
        """Handle one line; False closes the connection."""
        if cmd == "PING":
            await self.send("PONG")
            return True
        if cmd == "BYE":
            await self.stop_reply()
            await self.send("OK BYE")
            return False
        if cmd == "HELLO":
            return await self.hello(arg)
        if not self.st:
            await self.send("ERR PROTO Send HELLO first.")
            return True
        st = self.st
        st.last_seen = time.time()
        if cmd == "STOP":
            if not await self.stop_reply():
                await self.send("OK STOP")
            return True
        if self.task and not self.task.done():
            await self.send("ERR BUSY A reply is still coming; send STOP to end it.")
            return True
        if cmd == "MODELS":
            ms = self.cfg.usable_models()
            for m in ms:
                await self.send("MODEL %s %s %s" % (m["id"], m["tag"], m["label"]))
            await self.send("OK MODELS %d %s" % (len(ms), st.model["id"] if st.model else "-"))
        elif cmd == "USE":
            m = self.cfg.model(arg.strip())
            if not m:
                await self.send("ERR MODEL No model called %s here." % arg.strip()[:40])
            else:
                st.model = m
                await self.send("OK USE %s %s %s" % (m["id"], m["tag"], m["label"]))
        elif cmd == "SYSTEM":
            st.system = arg.strip() or None
            await self.send("OK SYSTEM")
        elif cmd == "NEW":
            st.history = []
            self.more = []
            await self.send("OK NEW")
        elif cmd == "MORE":
            if sum(len(x) for x in self.more) + len(arg) > self.cfg.r["max_turn_chars"]:
                self.more = []
                await self.send("ERR TOOLONG That message is too long.")
            else:
                self.more.append(arg)
                await self.send("OK MORE")
        elif cmd == "ASK":
            text = "\n".join(self.more + [arg]).strip()
            self.more = []
            if not text:
                await self.send("ERR PROTO Nothing to send.")
            elif len(text) > self.cfg.r["max_turn_chars"]:
                await self.send("ERR TOOLONG That message is too long (at most %d characters)."
                                % self.cfg.r["max_turn_chars"])
            elif not st.model:
                await self.send("ERR MODEL No model is available: check the relay's API keys.")
            else:
                why = self.relay.count_reply(st)
                if why:
                    await self.send("ERR LIMIT " + why)
                else:
                    self.cancel = asyncio.Event()
                    self.task = asyncio.create_task(self.reply(text, self.cancel))
        else:
            await self.send("ERR PROTO Unknown command %s." % cmd[:20])
        return True

    async def hello(self, arg):
        parts = arg.split()
        if self.st:
            await self.send("ERR PROTO Already said HELLO.")
            return True
        if not parts or not parts[0].isdigit() or int(parts[0]) < 1:
            await self.send("ERR PROTO HELLO needs a version: HELLO 1 <pin>.")
            return True
        pin = parts[1] if len(parts) > 1 else "-"
        want = str(self.cfg.r.get("pin") or "")
        given = "" if pin == "-" else pin
        if want and not hmac.compare_digest(given.encode(), want.encode()):
            self.wrong += 1
            self.relay.failures[self.ip].append(time.time())
            log("wrong PIN from", self.ip, "(%d)" % self.wrong)
            await asyncio.sleep(1.0)                         # slows down guessing
            if self.wrong >= self.cfg.r["pin_tries"] or self.relay.locked(self.ip):
                await self.send("BYE PIN Wrong PIN too many times.")
                return False
            await self.send("ERR PIN Wrong PIN.")
            return True
        self.relay.purge_sessions()
        resumed = False
        sid = parts[2].upper() if len(parts) > 2 else ""
        st = self.relay.sessions.get(sid)
        if st and not st.attached:
            resumed = True
        else:
            sid = secrets.token_hex(4).upper()
            st = SessionState(sid, self.cfg.default_model())
            self.relay.sessions[sid] = st
        if st.model and not self.cfg.model(st.model["id"]):
            st.model = self.cfg.default_model()
        st.attached = True
        st.last_seen = time.time()
        self.st = st
        log("hello", self.ip, "session", sid, "resumed" if resumed else "new")
        await self.send("OK HELLO %s %s %d %s" % (sid, "RESUMED" if resumed else "NEW",
                                                  sum(1 for m in st.history if m["role"] == "user"),
                                                  st.model["id"] if st.model else "-"))
        return True

    async def stop_reply(self):
        """End a running reply (its END line says "stopped"). True if one was running."""
        if self.task and not self.task.done():
            self.cancel.set()
            try:
                await asyncio.wait_for(asyncio.shield(self.task), 10)
            except asyncio.TimeoutError:
                self.task.cancel()
            return True
        return False

    def trimmed_history(self, text):
        """History plus the new turn, oldest pairs dropped to fit max_history_chars."""
        st = self.st
        limit = self.cfg.r["max_history_chars"]
        hist = list(st.history)
        dropped = 0
        while hist and sum(len(m["content"]) for m in hist) + len(text) > limit:
            hist = hist[2:] if len(hist) >= 2 else []
            dropped += 1
        if dropped:
            st.history = hist
        return hist + [{"role": "user", "content": text}], dropped

    async def reply(self, text, cancel):
        st = self.st
        model = st.model
        prov = self.cfg.providers[model["provider"]]
        system = st.system if st.system is not None else self.cfg.r["system_prompt"]
        messages, dropped = self.trimmed_history(text)
        info = {"stop": "done", "tokens": 0}
        out = []
        ps = textconv.PlainStream()
        t0 = time.time()
        sent = 0
        log("ask", self.ip, st.sid, model["id"], "%d chars" % len(text) + (": " + text if self.cfg.r["log_text"] else ""))
        try:
            if dropped:
                await self.send("NOTE Older messages were forgotten to make room.")
            await self.send("BEGIN %s %s" % (model["id"], model["tag"]))
            gen = prov.stream(model, system, messages, self.cfg.r["max_reply_tokens"], cancel, info)
            it = gen.__aiter__()
            pending = []                                # events held back up to FLUSH_S, to send fewer lines
            last = time.monotonic()
            nxt = None
            while True:
                if nxt is None:
                    nxt = asyncio.ensure_future(it.__anext__())
                wait = FLUSH_S - (time.monotonic() - last) if pending else None
                done, _ = await asyncio.wait({nxt}, timeout=max(0.0, wait) if wait is not None else None)
                if nxt not in done:                     # the model paused: send what we have
                    sent += await self.emit(pending)
                    pending, last = [], time.monotonic()
                    continue
                try:
                    delta = nxt.result()
                except StopAsyncIteration:
                    break
                finally:
                    if nxt in done:
                        nxt = None
                out.append(delta)
                pending += ps.feed(delta)
                if time.monotonic() - last >= FLUSH_S or sum(len(e[1]) for e in pending if e[0] == "T") >= 120:
                    sent += await self.emit(pending)
                    pending, last = [], time.monotonic()
                if cancel.is_set():
                    info["stop"] = "stopped"
                    break
            if nxt is not None:
                nxt.cancel()
            sent += await self.emit(pending)
            sent += await self.emit(ps.finish())
            if cancel.is_set():
                info["stop"] = "stopped"
            reply = "".join(out)
            if info["stop"] == "refused" and not reply.strip():
                await self.send("NOTE The model declined to answer that.")
            if reply.strip() and info["stop"] != "refused":
                st.history += [{"role": "user", "content": text}, {"role": "assistant", "content": reply}]
            await self.send("END %s %d %d" % (info["stop"], info.get("tokens") or 0, sent))
            log("end", st.sid, info["stop"], "%d tokens, %d bytes, %.1f s" % (info.get("tokens") or 0, sent,
                                                                                time.time() - t0))
        except providers.ProviderError as e:
            log("provider error", st.sid, e.code, e.message)
            try:
                await self.send("ERR %s %s" % (e.code, e.message))
            except (ConnectionError, OSError):
                pass
        except (ConnectionError, OSError):
            cancel.set()
        except asyncio.CancelledError:
            cancel.set()
            raise
        except Exception as e:                              # noqa: BLE001
            log("internal error", st.sid, repr(e))
            try:
                await self.send("ERR INTERNAL The relay hit a bug: %s" % type(e).__name__)
            except (ConnectionError, OSError):
                pass

    async def emit(self, events):
        n = 0
        for ev in textconv.pack(events):
            if ev[0] == "N":
                await self.send(b"N")
                n += 2
            else:
                b = textconv.to_mac(ev[1])
                if b:
                    await self.send(b"T " + b)
                    n += len(b) + 3
        return n

    async def cleanup(self):
        if self.task and not self.task.done():
            self.cancel.set()
            self.task.cancel()
        if self.st:
            self.st.attached = False
            self.st.last_seen = time.time()


def check(cfg):
    print("config:", cfg.path)
    print("listen: %s:%s   PIN: %s" % (cfg.r["host"], cfg.r["port"], "set" if cfg.r.get("pin") else "NONE"))
    for name, p in cfg.providers.items():
        ok, why = providers.available(p.cfg)
        print("provider %-12s %-18s %s (%s)" % (name, p.cfg.get("type"), "ready" if ok else "OFF", why))
    for m in cfg.models:
        ok, _ = providers.available(cfg.providers[m["provider"]].cfg)
        print("  model %-20s %-8s %-30s %s" % (m["id"], m["tag"], m["label"], "" if ok else "(hidden: no key)"))
    d = cfg.default_model()
    print("default model:", d["id"] if d else "none usable")


async def amain(a):
    raw = {} if a.mock_only else load_config(a.config)
    cfg = Config(raw, mock_only=a.mock_only)
    if a.host:
        cfg.r["host"] = a.host
    if a.port:
        cfg.r["port"] = a.port
    pin = a.pin if a.pin is not None else os.environ.get("ASKAI_PIN")
    if pin is not None:
        cfg.r["pin"] = pin
    if a.check:
        check(cfg)
        return
    if not cfg.usable_models():
        log("WARNING: no model is usable (no API keys set?). Run with --check.")
    if not cfg.r.get("pin") and cfg.r["host"] not in ("127.0.0.1", "localhost", "::1"):
        log("WARNING: no PIN set; anyone who can reach this port can use your API keys. Set ASKAI_PIN.")
    relay = Relay(cfg)
    server = await asyncio.start_server(relay.handle, cfg.r["host"], int(cfg.r["port"]))
    addrs = ", ".join("%s:%s" % s.getsockname()[:2] for s in server.sockets)
    log("Ask AI relay listening on", addrs, "| config", cfg.path,
        "| models:", ", ".join(m["id"] for m in cfg.usable_models()) or "none")
    async with server:
        await server.serve_forever()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--config", help="relay.toml (default: $ASKAI_CONFIG, ~/.askai/relay.toml, ./relay.toml, "
                                     "./relay.example.toml)")
    ap.add_argument("--host")
    ap.add_argument("--port", type=int)
    ap.add_argument("--pin", help="PIN the Mac must send (default: $ASKAI_PIN or the config's pin)")
    ap.add_argument("--check", action="store_true", help="show models and key status, then exit")
    ap.add_argument("--mock-only", action="store_true", help="only the built-in mock models (tests)")
    a = ap.parse_args()
    try:
        asyncio.run(amain(a))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

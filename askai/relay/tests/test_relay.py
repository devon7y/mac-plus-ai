#!/usr/bin/env python3
"""Offline tests for the Ask AI relay: Markdown rendering, Mac Roman, the provider adapters against a
local fake server replaying handwritten SSE streams (in each provider's documented format), and the
line protocol end to end with the mock provider. No network beyond 127.0.0.1, no real API keys:
the adapters read keys from test-only variable names (ASKAI_TEST_*), never the real ones.

    python3 askai/relay/tests/test_relay.py        (about 10 s)
"""
import asyncio
import http.server
import json
import os
import random
import socket
import sys
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import providers  # noqa: E402
import relay  # noqa: E402
import textconv  # noqa: E402

# ------------------------------------------------------------------------------ handwritten streams


def sse(events):
    """[(event name or None, json-able)] -> SSE text."""
    out = []
    for name, data in events:
        if name:
            out.append("event: %s\n" % name)
        out.append("data: %s\n\n" % (data if isinstance(data, str) else json.dumps(data)))
    return out


ANTHROPIC_OK = sse([
    ("message_start", {"type": "message_start", "message": {"id": "msg_01", "type": "message", "role": "assistant",
                                                             "model": "claude-test", "content": [],
                                                             "usage": {"input_tokens": 12, "output_tokens": 1}}}),
    ("content_block_start", {"type": "content_block_start", "index": 0, "content_block": {"type": "thinking",
                                                                                            "thinking": ""}}),
    ("content_block_delta", {"type": "content_block_delta", "index": 0,
                             "delta": {"type": "thinking_delta", "thinking": ""}}),
    ("content_block_stop", {"type": "content_block_stop", "index": 0}),
    ("content_block_start", {"type": "content_block_start", "index": 1, "content_block": {"type": "text", "text": ""}}),
    ("ping", {"type": "ping"}),
    ("content_block_delta", {"type": "content_block_delta", "index": 1, "delta": {"type": "text_delta", "text": "Hello"}}),
    ("content_block_delta", {"type": "content_block_delta", "index": 1,
                             "delta": {"type": "text_delta", "text": " **Mac**! Here's a list:\n\n- one"}}),
    ("content_block_delta", {"type": "content_block_delta", "index": 1, "delta": {"type": "text_delta", "text": "\n- two"}}),
    ("content_block_stop", {"type": "content_block_stop", "index": 1}),
    ("message_delta", {"type": "message_delta", "delta": {"stop_reason": "end_turn", "stop_sequence": None},
                       "usage": {"output_tokens": 17}}),
    ("message_stop", {"type": "message_stop"}),
])
ANTHROPIC_MAXTOK = sse([
    ("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "Cut"}}),
    ("message_delta", {"type": "message_delta", "delta": {"stop_reason": "max_tokens"}, "usage": {"output_tokens": 5}}),
    ("message_stop", {"type": "message_stop"}),
])
ANTHROPIC_REFUSAL = sse([
    ("message_delta", {"type": "message_delta", "delta": {"stop_reason": "refusal"},
                       "usage": {"output_tokens": 0}}),
    ("message_stop", {"type": "message_stop"}),
])
ANTHROPIC_MIDSTREAM_ERROR = sse([
    ("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "Par"}}),
    ("error", {"type": "error", "error": {"type": "overloaded_error", "message": "Overloaded"}}),
])
OPENAI_OK = sse([
    (None, {"id": "c1", "object": "chat.completion.chunk", "choices": [{"index": 0, "delta": {"role": "assistant",
                                                                                              "content": ""}}]}),
    (None, {"id": "c1", "choices": [{"index": 0, "delta": {"content": "Hi from "}}]}),
    (None, {"id": "c1", "choices": [{"index": 0, "delta": {"content": "*GPT*."}, "finish_reason": None}]}),
    (None, {"id": "c1", "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}),
    (None, {"id": "c1", "choices": [], "usage": {"prompt_tokens": 9, "completion_tokens": 4, "total_tokens": 13}}),
    (None, "[DONE]"),
])
OPENROUTER_OK = [": OPENROUTER PROCESSING\n\n"] + sse([
    (None, {"id": "g1", "choices": [{"index": 0, "delta": {"content": "Llama says hi"}}]}),
    (None, {"id": "g1", "choices": [{"index": 0, "delta": {}, "finish_reason": "length"}]}),
    (None, "[DONE]"),
])
GEMINI_OK = [s.replace("\n", "\r\n") for s in sse([
    (None, {"candidates": [{"content": {"parts": [{"text": "thinking...", "thought": True}], "role": "model"}}]}),
    (None, {"candidates": [{"content": {"parts": [{"text": "Gemini "}], "role": "model"}, "index": 0}]}),
    (None, {"candidates": [{"content": {"parts": [{"text": "here."}], "role": "model"}, "finishReason": "STOP",
                            "index": 0}], "usageMetadata": {"promptTokenCount": 5, "candidatesTokenCount": 3}}),
])]

ERRORS = {
    "/err401": (401, {"type": "error", "error": {"type": "authentication_error", "message": "invalid x-api-key"}}),
    "/err404": (404, {"error": {"message": "The model `nope` does not exist", "type": "invalid_request_error"}}),
    "/err429": (429, {"error": {"code": 429, "message": "Resource exhausted", "status": "RESOURCE_EXHAUSTED"}}),
    "/err529": (529, {"type": "error", "error": {"type": "overloaded_error", "message": "Overloaded"}}),
    "/err400": (400, {"type": "error", "error": {"type": "invalid_request_error",
                                                 "message": "max_tokens: must be at least 1"}}),
}


class FakeAPI(http.server.BaseHTTPRequestHandler):
    """Replays a stream chosen by the URL path prefix, chunked, with small gaps; records requests."""
    protocol_version = "HTTP/1.1"
    routes = {}
    requests = []

    def log_message(self, *a):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        FakeAPI.requests.append((self.path, dict(self.headers), json.loads(body)))
        key = self.path.split("/")[1]
        if "/" + key in ERRORS:
            code, j = ERRORS["/" + key]
            b = json.dumps(j).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)
            return
        chunks = FakeAPI.routes[key]
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        for c in chunks:
            b = c.encode()
            self.wfile.write(b"%x\r\n%s\r\n" % (len(b), b))
            self.wfile.flush()
            time.sleep(0.01)
        self.wfile.write(b"0\r\n\r\n")


def start_fake():
    FakeAPI.routes = {"anthropic": ANTHROPIC_OK, "amax": ANTHROPIC_MAXTOK, "arefuse": ANTHROPIC_REFUSAL,
                      "amid": ANTHROPIC_MIDSTREAM_ERROR, "openai": OPENAI_OK, "openrouter": OPENROUTER_OK,
                      "gemini": GEMINI_OK}
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeAPI)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, "http://127.0.0.1:%d" % srv.server_address[1]


async def collect(prov, model, messages, system="Be brief."):
    info = {}
    out = []
    async for d in prov.stream(model, system, messages, 64, asyncio.Event(), info):
        out.append(d)
    return "".join(out), info

# ------------------------------------------------------------------------------------------ tests


class TestText(unittest.TestCase):
    MD = ("# Title\n\nSome **bold**, *italic*, ~~gone~~ and `x_y` code; 2 * 3 = 6; snake_case.\n\n"
          "- item [link](http://a.b/c)\n  * sub\n3. three\n\n```js\nlet a = b * 2;\n  indented\n```\n\n"
          "| h1 | h2 |\n|:--|--:|\n| a | b |\n\n***\n> quote\nSetext\n===\nEnd.\n\n\n\n")
    WANT = ("Title\n\nSome bold, italic, gone and x_y code; 2 * 3 = 6; snake_case.\n\n"
            "• item link (http://a.b/c)\n  • sub\n3. three\n\n    let a = b * 2;\n      indented\n\n"
            "h1 | h2\na | b\n\n" + "—" * 12 + "\n> quote\nSetext\nEnd.")

    def test_render(self):
        self.assertEqual(textconv.render(self.MD), self.WANT)

    def test_chunking_does_not_matter(self):
        rng = random.Random(1)
        for chunk in (1, 2, 3, 5, 8, 13):
            self.assertEqual(textconv.render(self.MD, chunk), self.WANT, "chunk %d" % chunk)
        for _ in range(20):                      # random split points
            ps = textconv.PlainStream()
            evs, i = [], 0
            while i < len(self.MD):
                n = rng.randint(1, 9)
                evs += ps.feed(self.MD[i:i + n])
                i += n
            evs += ps.finish()
            self.assertEqual("".join("\n" if e[0] == "N" else e[1] for e in evs), self.WANT)

    def test_streams_word_by_word(self):
        ps = textconv.PlainStream()
        self.assertEqual(ps.feed("Hello wor"), [("T", "Hello ")])
        self.assertEqual(ps.feed("ld and"), [("T", "world ")])
        self.assertEqual(ps.feed(" a [link"), [("T", "and a ")])         # an open [ waits
        self.assertEqual(ps.feed("](u) x"), [("T", "link (u) ")])
        self.assertEqual(ps.finish(), [("T", "x")])

    def test_long_text_split(self):
        evs = textconv.PlainStream().feed("word " * 200 + "\n")
        self.assertTrue(all(len(textconv.to_mac(e[1])) <= 200 for e in evs if e[0] == "T"))

    def test_mac_roman(self):
        self.assertEqual(textconv.to_mac("“Hi” — café → 5×3 € •"),
                         b"\xd2Hi\xd3 \xd1 caf\x8e -> 5x3 EUR \xa5")
        self.assertEqual(textconv.to_mac("日"), b"?")
        self.assertEqual(textconv.render("Nice \U0001F600 work \U0001F44D\U0001F3FD!"), "Nice work !")
        self.assertEqual(textconv.from_mac(b"caf\x8e\rnext"), "café\nnext")


class TestProviders(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv, cls.base = start_fake()
        os.environ["ASKAI_TEST_KEY"] = "test-key-not-real"

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()

    def prov(self, typ, path, **kw):
        cfg = dict(type=typ, base_url=self.base + "/" + path, api_key_env="ASKAI_TEST_KEY", **kw)
        return providers.make("t", cfg)

    def test_anthropic(self):
        FakeAPI.requests.clear()
        p = self.prov("anthropic", "anthropic")
        msgs = [{"role": "user", "content": "hi"}, {"role": "assistant", "content": "yo"},
                {"role": "user", "content": "more"}]
        text, info = asyncio.run(collect(p, {"model": "claude-opus-5-5", "fallbacks": "default", "effort": "low"},
                                         msgs))
        self.assertEqual(text, "Hello **Mac**! Here's a list:\n\n- one\n- two")
        self.assertEqual(info, {"stop": "done", "tokens": 17})
        path, headers, body = FakeAPI.requests[-1]
        self.assertEqual(path, "/anthropic/v1/messages")
        h = {k.lower(): v for k, v in headers.items()}
        self.assertEqual(h["x-api-key"], "test-key-not-real")
        self.assertEqual(h["anthropic-version"], "2026-10-21")
        self.assertEqual(h["anthropic-beta"], "server-side-fallback-2026-07-01")
        self.assertEqual(body["model"], "claude-opus-5-5")
        self.assertEqual(body["fallbacks"], "default")
        self.assertEqual(body["output_config"], {"effort": "low"})
        self.assertTrue(body["stream"])
        self.assertEqual(body["system"], "Be brief.")
        self.assertEqual(body["messages"], msgs)
        self.assertNotIn("temperature", body)

    def test_anthropic_stops(self):
        _, info = asyncio.run(collect(self.prov("anthropic", "amax"), {"model": "m"}, [{"role": "user", "content": "x"}]))
        self.assertEqual(info["stop"], "length")
        text, info = asyncio.run(collect(self.prov("anthropic", "arefuse"), {"model": "m"},
                                         [{"role": "user", "content": "x"}]))
        self.assertEqual((text, info["stop"]), ("", "refused"))
        with self.assertRaises(providers.ProviderError) as cm:
            asyncio.run(collect(self.prov("anthropic", "amid"), {"model": "m"}, [{"role": "user", "content": "x"}]))
        self.assertEqual(cm.exception.code, "PROVIDER")
        self.assertIn("overloaded", cm.exception.message)

    def test_openai(self):
        FakeAPI.requests.clear()
        text, info = asyncio.run(collect(self.prov("openai", "openai"), {"model": "gpt-x"},
                                         [{"role": "user", "content": "hi"}]))
        self.assertEqual((text, info), ("Hi from *GPT*.", {"stop": "done", "tokens": 4}))
        path, headers, body = FakeAPI.requests[-1]
        self.assertEqual(path, "/openai/chat/completions")
        self.assertEqual({k.lower(): v for k, v in headers.items()}["authorization"], "Bearer test-key-not-real")
        self.assertEqual(body["messages"][0], {"role": "system", "content": "Be brief."})
        self.assertEqual(body["max_completion_tokens"], 64)
        self.assertEqual(body["stream_options"], {"include_usage": True})

    def test_openrouter_and_local(self):
        FakeAPI.requests.clear()
        p = self.prov("openai_compatible", "openrouter", headers={"X-Title": "Ask AI"})
        text, info = asyncio.run(collect(p, {"model": "meta/llama"}, [{"role": "user", "content": "hi"}]))
        self.assertEqual((text, info["stop"]), ("Llama says hi", "length"))
        _, headers, body = FakeAPI.requests[-1]
        self.assertEqual({k.lower(): v for k, v in headers.items()}["x-title"], "Ask AI")
        self.assertEqual(body["max_tokens"], 64)
        self.assertNotIn("stream_options", body)
        local = providers.make("l", {"type": "openai_compatible", "base_url": self.base + "/openrouter"})
        text, _ = asyncio.run(collect(local, {"model": "llama3"}, [{"role": "user", "content": "hi"}]))
        self.assertEqual(text, "Llama says hi")
        self.assertNotIn("authorization", {k.lower() for k in FakeAPI.requests[-1][1]})

    def test_gemini(self):
        FakeAPI.requests.clear()
        msgs = [{"role": "user", "content": "hi"}, {"role": "assistant", "content": "yo"},
                {"role": "user", "content": "again"}]
        text, info = asyncio.run(collect(self.prov("gemini", "gemini"), {"model": "gemini-x"}, msgs))
        self.assertEqual((text, info), ("Gemini here.", {"stop": "done", "tokens": 3}))
        path, headers, body = FakeAPI.requests[-1]
        self.assertEqual(path, "/gemini/v1beta/models/gemini-x:streamGenerateContent?alt=sse")
        self.assertEqual({k.lower(): v for k, v in headers.items()}["x-goog-api-key"], "test-key-not-real")
        self.assertEqual([c["role"] for c in body["contents"]], ["user", "model", "user"])
        self.assertEqual(body["systemInstruction"], {"parts": [{"text": "Be brief."}]})

    def test_http_errors(self):
        want = {"err401": "KEY", "err404": "MODEL", "err429": "LIMIT", "err529": "PROVIDER", "err400": "PROVIDER"}
        for path, code in want.items():
            with self.assertRaises(providers.ProviderError) as cm:
                asyncio.run(collect(self.prov("anthropic", path), {"model": "m"}, [{"role": "user", "content": "x"}]))
            self.assertEqual(cm.exception.code, code, path)
        self.assertIn("max_tokens: must be at least 1", cm.exception.message)

    def test_missing_key_and_network(self):
        p = providers.make("t", {"type": "anthropic", "api_key_env": "ASKAI_TEST_UNSET_KEY", "base_url": self.base})
        os.environ.pop("ASKAI_TEST_UNSET_KEY", None)
        with self.assertRaises(providers.ProviderError) as cm:
            asyncio.run(collect(p, {"model": "m"}, [{"role": "user", "content": "x"}]))
        self.assertEqual(cm.exception.code, "KEY")
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()                                   # nothing listens there
        p = providers.make("t", {"type": "openai_compatible", "base_url": "http://127.0.0.1:%d/v1" % port})
        with self.assertRaises(providers.ProviderError) as cm:
            asyncio.run(collect(p, {"model": "m"}, [{"role": "user", "content": "x"}]))
        self.assertEqual(cm.exception.code, "NET")


# --------------------------------------------------------------------------------- protocol tests

class Client:
    def __init__(self, reader, writer):
        self.r, self.w = reader, writer
        self.buf = b""

    async def line(self, timeout=10):
        while b"\r" not in self.buf:
            data = await asyncio.wait_for(self.r.read(4096), timeout)
            if not data:
                raise EOFError(self.buf)
            self.buf += data
        line, self.buf = self.buf.split(b"\r", 1)
        return line

    async def send(self, s):
        self.w.write((s if isinstance(s, bytes) else s.encode("mac_roman")) + b"\r")
        await self.w.drain()

    async def until(self, prefix, timeout=15, stop_on_err=True):
        got = []
        while True:
            ln = await self.line(timeout)
            got.append(ln)
            if ln.startswith(prefix.encode()) or (stop_on_err and ln.startswith(b"ERR")):
                return got

    async def reply(self):
        lines = await self.until("END")
        text = b""
        for ln in lines:
            if ln.startswith(b"T "):
                text += ln[2:]
            elif ln == b"N":
                text += b"\r"
        return lines, text.decode("mac_roman")


class TestProtocol(unittest.TestCase):
    def run_relay(self, coro_fn, extra=None, raw=None):
        async def main():
            cfgraw = raw or {
                "relay": dict({"name": "Test relay", "pin": "4321", "default_model": "mock",
                               "replies_per_minute": 50, "max_turn_chars": 300}, **(extra or {})),
                "provider": {"mock": {"type": "mock", "latency": 0.05, "chars_per_second": 5000},
                             "slow": {"type": "mock", "latency": 0.05, "chars_per_second": 60}},
                "model": [{"id": "mock", "label": "Mock Model", "tag": "Mock", "provider": "mock", "model": "m1"},
                          {"id": "slowmock", "label": "Slow “Mock”", "tag": "Slow", "provider": "slow",
                           "model": "m2"}]}
            r = relay.Relay(relay.Config(cfgraw))
            srv = await asyncio.start_server(r.handle, "127.0.0.1", 0)
            port = srv.sockets[0].getsockname()[1]

            async def connect():
                rd, wr = await asyncio.open_connection("127.0.0.1", port)
                return Client(rd, wr)
            try:
                await coro_fn(connect)
            finally:
                srv.close()
        stderr = sys.stderr
        sys.stderr = open(os.devnull, "w")              # the relay's log
        try:
            asyncio.run(main())
        finally:
            sys.stderr.close()
            sys.stderr = stderr

    def test_session(self):
        async def t(connect):
            c = await connect()
            self.assertEqual(await c.line(), b"ASKAI 1 Test relay")
            await c.send("MODELS")
            self.assertTrue((await c.line()).startswith(b"ERR PROTO"))
            await c.send("HELLO 1 1111")
            self.assertEqual(await c.line(), b"ERR PIN Wrong PIN.")
            await c.send("HELLO 1 4321")
            ok = (await c.line()).split()
            self.assertEqual(ok[:2] + ok[3:], [b"OK", b"HELLO", b"NEW", b"0", b"mock"])
            sid = ok[2].decode()
            await c.send("MODELS")
            lines = await c.until("OK MODELS")
            self.assertEqual(lines, [b"MODEL mock Mock Mock Model", b"MODEL slowmock Slow Slow \xd2Mock\xd3",
                                     b"OK MODELS 2 mock"])
            await c.send("USE nope")
            self.assertTrue((await c.line()).startswith(b"ERR MODEL"))
            await c.send("SYSTEM Answer in one line.")
            self.assertEqual(await c.line(), b"OK SYSTEM")
            await c.send(b"ASK hello Mac \x8e")                      # Mac Roman e-acute
            lines, text = await c.reply()
            self.assertEqual(lines[0], b"BEGIN mock Mock")
            self.assertTrue(text.startswith('You said: "hello Mac é"\r\rHello from the mock provider!'), text)
            self.assertTrue(lines[-1].startswith(b"END done "))
            self.assertTrue(all(len(x) <= 203 for x in lines))
            # two-line message with MORE, then the second canned reply (a list with bullets)
            await c.send("MORE first line")
            self.assertEqual(await c.line(), b"OK MORE")
            await c.send("ASK second line")
            _, text = await c.reply()
            self.assertIn("• Writing letters in MacWrite", text)
            self.assertIn("turn 2", text)
            # provider error: a clean ERR, history unchanged
            await c.send("ASK please !error")
            lines = await c.until("END")
            self.assertTrue(lines[-1].startswith(b"ERR PROVIDER"), lines)
            await c.send("ASK !cut")
            lines, _ = await c.reply()
            self.assertTrue(lines[-1].startswith(b"END length"))
            await c.send("ASK !refuse")
            lines, _ = await c.reply()
            self.assertIn(b"NOTE The model declined to answer that.", lines)
            self.assertTrue(lines[-1].startswith(b"END refused"))
            await c.send("PING")
            self.assertEqual(await c.line(), b"PONG")
            # the line drops (no BYE); resume with the session id: history is kept
            c.w.close()
            await asyncio.sleep(0.2)
            c = await connect()
            await c.line()
            await c.send("HELLO 1 4321 " + sid)
            self.assertEqual((await c.line()).split()[2:5], [sid.encode(), b"RESUMED", b"3"])
            await c.send("NEW")
            self.assertEqual(await c.line(), b"OK NEW")
            await c.send("BYE")
            self.assertEqual(await c.line(), b"OK BYE")
        self.run_relay(t)

    def test_stop_busy_and_limits(self):
        async def t(connect):
            c = await connect()
            await c.line()
            await c.send("HELLO 1 4321")
            await c.line()
            await c.send("USE slowmock")
            self.assertEqual(await c.line(), b"OK USE slowmock Slow Slow \xd2Mock\xd3")
            await c.send("ASK !long")
            self.assertEqual(await c.line(), b"BEGIN slowmock Slow")
            await c.line()
            await c.send("ASK another")                             # while streaming
            await c.send("STOP")
            lines = await c.until("END", stop_on_err=False)
            self.assertIn(b"ERR BUSY A reply is still coming; send STOP to end it.", lines)
            self.assertTrue(lines[-1].startswith(b"END stopped"), lines[-1])
            await c.send("ASK " + "x" * 400)
            self.assertTrue((await c.line()).startswith(b"ERR TOOLONG"))
            await c.send(b"X" * 5000)                                  # longer than max_line_bytes
            self.assertEqual(await c.line(), b"ERR TOOLONG That line was too long.")
            await c.send("PING")
            self.assertEqual(await c.line(), b"PONG")
        self.run_relay(t)

    def test_pin_lockout_and_rate_limit(self):
        async def t(connect):
            c = await connect()
            await c.line()
            for _ in range(2):
                await c.send("HELLO 1 0000")
                self.assertEqual(await c.line(), b"ERR PIN Wrong PIN.")
            await c.send("HELLO 1 0000")
            self.assertEqual(await c.line(), b"BYE PIN Wrong PIN too many times.")
            c = await connect()
            self.assertTrue((await c.line()).startswith(b"BYE LOCKED"))
        self.run_relay(t, {"lockout_failures": 3})

        async def t2(connect):
            c = await connect()
            await c.line()
            await c.send("HELLO 1 4321")
            await c.line()
            for i in range(2):
                await c.send("ASK hi %d" % i)
                await c.reply()
            await c.send("ASK hi again")
            self.assertTrue((await c.line()).startswith(b"ERR LIMIT Slow down"))
        self.run_relay(t2, {"replies_per_minute": 2})

    def test_history_trim(self):
        async def t(connect):
            c = await connect()
            await c.line()
            await c.send("HELLO 1 4321")
            await c.line()
            for i in range(4):
                await c.send("ASK message number %d" % i)
                lines, _ = await c.reply()
            self.assertIn(b"NOTE Older messages were forgotten to make room.", lines)
        self.run_relay(t, {"max_history_chars": 700})


class TestWithFakeAnthropic(unittest.TestCase):
    """The relay with a real adapter (Anthropic) talking to the fake API: Markdown arrives as plain text."""

    def test_chain(self):
        srv, base = start_fake()
        os.environ["ASKAI_TEST_KEY"] = "test-key-not-real"

        async def main():
            raw = {"relay": {"name": "R", "pin": ""},
                   "provider": {"a": {"type": "anthropic", "base_url": base + "/anthropic",
                                      "api_key_env": "ASKAI_TEST_KEY"}},
                   "model": [{"id": "claude", "tag": "Claude", "label": "Claude", "provider": "a",
                              "model": "claude-opus-5-5"}]}
            r = relay.Relay(relay.Config(raw))
            s = await asyncio.start_server(r.handle, "127.0.0.1", 0)
            rd, wr = await asyncio.open_connection("127.0.0.1", s.sockets[0].getsockname()[1])
            c = Client(rd, wr)
            await c.line()
            await c.send("HELLO 1 -")
            await c.line()
            await c.send("ASK hi")
            lines, text = await c.reply()
            s.close()
            return lines, text
        stderr, sys.stderr = sys.stderr, open(os.devnull, "w")
        try:
            lines, text = asyncio.run(main())
        finally:
            sys.stderr.close()
            sys.stderr = stderr
        srv.shutdown()
        self.assertEqual(text, "Hello Mac! Here's a list:\r\r• one\r• two")
        self.assertEqual(lines[-1], b"END done 17 %d" % sum(len(x) + 1 for x in lines[1:-1]))


if __name__ == "__main__":
    unittest.main(verbosity=1)

"""Model providers for the Ask AI relay (standard library only).

Every provider turns (system prompt, conversation, max tokens) into a stream of text deltas:

    async for delta in provider.stream(model, system, messages, max_tokens, cancel):
        ...
    provider_info = the dict passed in as ``info`` ("stop": done|length|refused|stopped, "tokens": n)

``messages`` is a list of {"role": "user"|"assistant", "content": str}, oldest first, ending with the
new user turn. Errors raise ProviderError(code, message) with code KEY, MODEL, LIMIT, NET or PROVIDER.

Types (``type`` in relay.toml):
  mock               canned replies with realistic timing (tests; no network)
  anthropic          Anthropic Messages API (POST /v1/messages, SSE)
  openai             OpenAI Chat Completions (POST /chat/completions, SSE)
  openai_compatible  the same wire format: OpenRouter, Ollama, LM Studio, llama.cpp, vLLM ...
  gemini             Google Gemini (POST /v1beta/models/<m>:streamGenerateContent?alt=sse)

HTTPS requests run in a worker thread (urllib) and hand SSE events to the event loop, so a slow
provider never blocks other connections. API keys are read from the environment variable named in the
provider's config at request time and are never logged.
"""
import asyncio
import json
import os
import random
import ssl
import threading
import urllib.error
import urllib.request

ANTHROPIC_VERSION = "2026-10-21"
FALLBACK_BETA = "server-side-fallback-2026-07-01"
USER_AGENT = "AskAI-relay/1.0 (+https://github.com/devon7y/mac-plus-ai)"


class ProviderError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


def _short(msg, n=200):
    msg = " ".join(str(msg).split())
    return msg if len(msg) <= n else msg[:n - 1] + "…"


def _http_error(status, body):
    """ProviderError for an HTTP error status and its body (JSON error shapes of all providers)."""
    detail = ""
    try:
        j = json.loads(body.decode("utf-8", "replace"))
        e = j.get("error", j) if isinstance(j, dict) else j
        if isinstance(e, dict):
            detail = e.get("message") or e.get("type") or ""
        elif isinstance(e, list) and e and isinstance(e[0], dict):   # Gemini can wrap errors in a list
            ee = e[0].get("error", e[0])
            detail = ee.get("message", "")
        elif isinstance(e, str):
            detail = e
    except (ValueError, AttributeError):
        detail = body.decode("utf-8", "replace")[:200]
    if status in (401, 403):
        return ProviderError("KEY", "The provider rejected the API key (HTTP %d). %s" % (status, _short(detail, 120)))
    if status == 404:
        return ProviderError("MODEL", "The provider doesn't know this model (HTTP 404). %s" % _short(detail, 120))
    if status == 429:
        return ProviderError("LIMIT", "The provider's rate limit was hit; try again in a minute. %s" % _short(detail, 100))
    if status in (500, 502, 503, 504, 529):
        return ProviderError("PROVIDER", "The provider is busy or down (HTTP %d); try again soon." % status)
    return ProviderError("PROVIDER", "Provider error (HTTP %d): %s" % (status, _short(detail, 160)))


def _sse_thread(url, headers, body, timeout, cancel_flag, put):
    """Worker thread: POST and parse the SSE stream, putting ("event", name, data) items."""
    try:
        req = urllib.request.Request(url, data=json.dumps(body).encode("utf-8"), method="POST",
                                     headers=dict(headers, **{"Content-Type": "application/json",
                                                              "Accept": "text/event-stream",
                                                              "User-Agent": USER_AGENT}))
        ctx = ssl.create_default_context() if url.startswith("https:") else None
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
            event, data = None, []
            while not cancel_flag.is_set():
                raw = resp.readline()
                if not raw:
                    break
                line = raw.decode("utf-8", "replace").rstrip("\r\n")
                if not line:
                    if data:
                        put(("event", event or "message", "\n".join(data)))
                    event, data = None, []
                    continue
                if line.startswith(":"):
                    continue                            # SSE comment (keep-alives, OpenRouter notes)
                field, _, value = line.partition(":")
                if value.startswith(" "):
                    value = value[1:]
                if field == "event":
                    event = value
                elif field == "data":
                    data.append(value)
            if data and not cancel_flag.is_set():
                put(("event", event or "message", "\n".join(data)))
        put(("done",))
    except urllib.error.HTTPError as e:
        try:
            b = e.read()[:8000]
        except Exception:                               # noqa: BLE001
            b = b""
        put(("http", e.code, b))
    except Exception as e:                              # noqa: BLE001  (URLError, timeouts, TLS ...)
        put(("exc", e))


async def sse_events(url, headers, body, timeout, cancel):
    """Async generator of (event name, data string) from an SSE POST; raises ProviderError."""
    loop = asyncio.get_running_loop()
    q = asyncio.Queue()
    flag = threading.Event()

    def put(item):
        try:
            loop.call_soon_threadsafe(q.put_nowait, item)
        except RuntimeError:                            # the loop has closed (the relay is stopping)
            pass
    threading.Thread(target=_sse_thread, args=(url, headers, body, timeout, flag, put), daemon=True).start()
    try:
        while True:
            get = asyncio.ensure_future(q.get())
            stop = asyncio.ensure_future(cancel.wait())
            done, _ = await asyncio.wait({get, stop}, return_when=asyncio.FIRST_COMPLETED)
            if get not in done:
                get.cancel()
                return
            stop.cancel()
            item = get.result()
            if item[0] == "event":
                yield item[1], item[2]
            elif item[0] == "done":
                return
            elif item[0] == "http":
                raise _http_error(item[1], item[2])
            else:
                e = item[1]
                reason = getattr(e, "reason", e)
                raise ProviderError("NET", "Couldn't reach the provider: %s" % _short(reason, 140))
    finally:
        flag.set()


def _key(pcfg):
    env = pcfg.get("api_key_env")
    if not env:
        return ""
    k = os.environ.get(env, "")
    if not k:
        raise ProviderError("KEY", "No API key: set %s where the relay runs." % env)
    return k


def available(pcfg):
    """(ok, why) without revealing anything about a key except whether it is set."""
    t = pcfg.get("type")
    if t == "mock":
        return True, "built in"
    env = pcfg.get("api_key_env")
    if env and not os.environ.get(env):
        return False, "%s is not set" % env
    return True, ("%s is set" % env) if env else "no key needed"


class Provider:
    def __init__(self, name, cfg):
        self.name = name
        self.cfg = cfg
        self.timeout = float(cfg.get("timeout", 120))


# ------------------------------------------------------------------------------------------- mock

MOCK_REPLIES = [
    "Hello from the mock provider! I'm a pretend model that lives inside the relay, so nothing here "
    "costs money.\n\nYou can ask me anything; I'll answer with **canned** text, but the words travel "
    "the whole way: relay, modem, serial cable and your Mac's screen.",
    "Here are three things a Macintosh Plus is good at:\n\n- Writing letters in *MacWrite*\n"
    "- Drawing in MacPaint\n- Chatting with a model through a modem\n\nThe Plus came out in January "
    "1986 with 1 MB of memory and an 800K floppy drive.",
    "A tiny program, as the relay shows code:\n\n```c\nint add(int a, int b)\n{\n    return a + b;\n}\n"
    "```\n\nCode blocks arrive indented by four spaces, and `inline code` loses its backticks.",
    "### A short answer\n\nYes. The [relay](https://example.com/relay) turns Markdown into plain "
    "text, keeps the conversation and sends only the new words over the line.",
]
MOCK_LONG = " ".join(
    "Sentence %d of a long reply, written to check scrolling and flow control on the serial line." % i
    for i in range(1, 41))


class MockProvider(Provider):
    """Canned replies. Triggers in the user's text: "!error" (provider error), "!long" (about 4 KB),
    "!slow" (slow streaming), "!key" (missing key), "!refuse" (a refusal), "!cut" (length limit)."""

    async def stream(self, model, system, messages, max_tokens, cancel, info):
        text = messages[-1]["content"]
        low = text.lower()
        turn = sum(1 for m in messages if m["role"] == "user")
        speed = float(self.cfg.get("chars_per_second", 250))
        latency = float(self.cfg.get("latency", 0.6))
        info["stop"] = "done"
        if "!key" in low:
            raise ProviderError("KEY", "No API key: set MOCK_API_KEY where the relay runs.")
        await asyncio.sleep(latency)
        if "!error" in low:
            raise ProviderError("PROVIDER", "The mock provider failed on purpose (you typed !error).")
        if "!refuse" in low:
            info["stop"] = "refused"
            return
        if "!long" in low:
            reply = MOCK_LONG
        else:
            shown = text if len(text) <= 60 else text[:57] + "..."
            reply = 'You said: "%s"\n\n' % shown + MOCK_REPLIES[(turn - 1) % len(MOCK_REPLIES)]
            if model.get("model"):
                reply += "\n\n(Mock model: %s, turn %d.)" % (model["model"], turn)
        if "!slow" in low:
            speed = 20.0
        if "!cut" in low:
            reply = reply[:120]
            info["stop"] = "length"
        rng = random.Random(turn)
        i = 0
        while i < len(reply):
            if cancel.is_set():
                info["stop"] = "stopped"
                return
            n = rng.randint(2, 12)                     # a token or three at a time
            chunk = reply[i:i + n]
            i += n
            info["tokens"] = info.get("tokens", 0) + 1
            yield chunk
            await asyncio.sleep(len(chunk) / speed)


# -------------------------------------------------------------------------------------- Anthropic

class AnthropicProvider(Provider):
    def request(self, model, system, messages, max_tokens):
        base = self.cfg.get("base_url", "https://api.anthropic.com").rstrip("/")
        headers = {"x-api-key": _key(self.cfg), "anthropic-version": self.cfg.get("version", ANTHROPIC_VERSION)}
        body = {"model": model["model"], "max_tokens": int(max_tokens), "stream": True,
                "messages": [{"role": m["role"], "content": m["content"]} for m in messages]}
        if system:
            body["system"] = system
        if model.get("effort"):
            body["output_config"] = {"effort": model["effort"]}
        if model.get("fallbacks"):
            body["fallbacks"] = model["fallbacks"]
            headers["anthropic-beta"] = FALLBACK_BETA
        return base + "/v1/messages", headers, body

    async def stream(self, model, system, messages, max_tokens, cancel, info):
        url, headers, body = self.request(model, system, messages, max_tokens)
        info["stop"] = "done"
        async for event, data in sse_events(url, headers, body, self.timeout, cancel):
            try:
                j = json.loads(data)
            except ValueError:
                continue
            t = j.get("type", event)
            if t == "content_block_delta":
                d = j.get("delta", {})
                if d.get("type") == "text_delta" and d.get("text"):
                    yield d["text"]
            elif t == "message_delta":
                sr = (j.get("delta") or {}).get("stop_reason")
                if sr:
                    info["stop"] = {"max_tokens": "length", "refusal": "refused"}.get(sr, "done")
                u = j.get("usage") or {}
                if "output_tokens" in u:
                    info["tokens"] = u["output_tokens"]
            elif t == "error":
                e = j.get("error") or {}
                et = e.get("type", "")
                if et == "overloaded_error":
                    raise ProviderError("PROVIDER", "Anthropic is overloaded right now; try again soon.")
                if et == "rate_limit_error":
                    raise ProviderError("LIMIT", "Anthropic's rate limit was hit; try again in a minute.")
                raise ProviderError("PROVIDER", "Anthropic error: %s" % _short(e.get("message") or et, 160))
        if cancel.is_set():
            info["stop"] = "stopped"


# ----------------------------------------------------------------------------------- OpenAI style

class OpenAIProvider(Provider):
    """OpenAI Chat Completions and every server that speaks it (OpenRouter, Ollama, LM Studio)."""

    def request(self, model, system, messages, max_tokens):
        default = "https://api.openai.com/v1" if self.cfg.get("type") == "openai" else "http://localhost:11434/v1"
        base = self.cfg.get("base_url", default).rstrip("/")
        headers = {}
        k = _key(self.cfg)
        if k:
            headers["Authorization"] = "Bearer " + k
        for hk, hv in (self.cfg.get("headers") or {}).items():
            headers[str(hk)] = str(hv)
        msgs = ([{"role": "system", "content": system}] if system else []) + \
            [{"role": m["role"], "content": m["content"]} for m in messages]
        body = {"model": model["model"], "messages": msgs, "stream": True}
        field = self.cfg.get("max_tokens_field",
                             "max_completion_tokens" if self.cfg.get("type") == "openai" else "max_tokens")
        if field:
            body[field] = int(max_tokens)
        if self.cfg.get("stream_usage", self.cfg.get("type") == "openai"):
            body["stream_options"] = {"include_usage": True}
        if model.get("effort") and self.cfg.get("type") == "openai":
            body["reasoning_effort"] = model["effort"]
        for bk, bv in (model.get("extra_body") or {}).items():
            body[bk] = bv
        return base + "/chat/completions", headers, body

    async def stream(self, model, system, messages, max_tokens, cancel, info):
        url, headers, body = self.request(model, system, messages, max_tokens)
        info["stop"] = "done"
        async for _event, data in sse_events(url, headers, body, self.timeout, cancel):
            if data.strip() == "[DONE]":
                break
            try:
                j = json.loads(data)
            except ValueError:
                continue
            if "error" in j and j["error"]:
                e = j["error"]
                raise ProviderError("PROVIDER", "Provider error: %s" % _short(
                    e.get("message") if isinstance(e, dict) else e, 160))
            for ch in j.get("choices") or []:
                d = ch.get("delta") or {}
                if d.get("content"):
                    yield d["content"]
                fr = ch.get("finish_reason")
                if fr:
                    info["stop"] = {"length": "length", "content_filter": "refused"}.get(fr, "done")
            u = j.get("usage") or {}
            if u.get("completion_tokens") is not None:
                info["tokens"] = u["completion_tokens"]
        if cancel.is_set():
            info["stop"] = "stopped"


# ----------------------------------------------------------------------------------------- Gemini

class GeminiProvider(Provider):
    def request(self, model, system, messages, max_tokens):
        base = self.cfg.get("base_url", "https://generativelanguage.googleapis.com").rstrip("/")
        headers = {"x-goog-api-key": _key(self.cfg)}
        body = {"contents": [{"role": "model" if m["role"] == "assistant" else "user",
                              "parts": [{"text": m["content"]}]} for m in messages],
                "generationConfig": {"maxOutputTokens": int(max_tokens)}}
        if system:
            body["systemInstruction"] = {"parts": [{"text": system}]}
        for bk, bv in (model.get("extra_body") or {}).items():
            body[bk] = bv
        return "%s/v1beta/models/%s:streamGenerateContent?alt=sse" % (base, model["model"]), headers, body

    async def stream(self, model, system, messages, max_tokens, cancel, info):
        url, headers, body = self.request(model, system, messages, max_tokens)
        info["stop"] = "done"
        async for _event, data in sse_events(url, headers, body, self.timeout, cancel):
            try:
                j = json.loads(data)
            except ValueError:
                continue
            if isinstance(j, dict) and j.get("error"):
                raise ProviderError("PROVIDER", "Gemini error: %s" % _short(j["error"].get("message", ""), 160))
            fb = j.get("promptFeedback") or {}
            if fb.get("blockReason"):
                info["stop"] = "refused"
            for c in j.get("candidates") or []:
                for p in (c.get("content") or {}).get("parts") or []:
                    if p.get("text") and not p.get("thought"):
                        yield p["text"]
                fr = c.get("finishReason")
                if fr:
                    info["stop"] = ("done" if fr == "STOP" else "length" if fr == "MAX_TOKENS" else "refused")
            u = j.get("usageMetadata") or {}
            if u.get("candidatesTokenCount") is not None:
                info["tokens"] = u["candidatesTokenCount"]
        if cancel.is_set():
            info["stop"] = "stopped"


TYPES = {"mock": MockProvider, "anthropic": AnthropicProvider, "openai": OpenAIProvider,
         "openai_compatible": OpenAIProvider, "gemini": GeminiProvider}


def make(name, cfg):
    t = cfg.get("type")
    if t not in TYPES:
        raise ValueError("provider %s: unknown type %r (one of %s)" % (name, t, ", ".join(TYPES)))
    return TYPES[t](name, cfg)

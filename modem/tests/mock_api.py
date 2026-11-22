#!/usr/bin/env python3
"""A fake model API for the end-to-end test: answers the Anthropic, OpenAI and Gemini streaming
endpoints over plain HTTP with chunked SSE, and checks each request the modem sends.

    python3 -I modem/tests/mock_api.py PORT         (prints "ready" when listening)

Requests that break a rule get an HTTP 400 whose message names the rule, so the modem's ERR line shows it.
"""
import http.server
import json
import sys
import time

TEXT = ["Hello **from** the", " fake API: “quotes”, an", " emoji \U0001F600 and\n\n- a", " list\n- of two"]


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def fail(self, why):
        body = json.dumps({"type": "error", "error": {"type": "invalid_request_error", "message": why}}).encode()
        self.send_response(400)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        try:
            body = json.loads(self.rfile.read(n))
        except ValueError:
            return self.fail("the body isn't JSON")
        for k in ("temperature", "top_p", "top_k", "thinking"):
            if k in body:
                return self.fail("sent %s" % k)
        path = self.path
        if path == "/v1/messages":
            if self.headers.get("anthropic-version") != "2026-10-21":
                return self.fail("anthropic-version is %s" % self.headers.get("anthropic-version"))
            if self.headers.get("x-api-key") != "test-key-anthropic":
                return self.fail("bad x-api-key")
            fb = "fallbacks" in body
            if fb != (body["model"] in ("claude-opus-5-5", "claude-sonnet-5-5")) or \
                    fb != (self.headers.get("anthropic-beta") == "server-side-fallback-2026-07-01"):
                return self.fail("fallbacks/beta header wrong for %s" % body["model"])
            if body["messages"][-1]["role"] != "user" or body.get("stream") is not True:
                return self.fail("bad messages or no stream")
            events = [("message_start", {"type": "message_start", "message": {"id": "m", "usage": {"output_tokens": 1}}}),
                      ("content_block_start", {"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}})]
            events += [("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": t}}) for t in TEXT]
            events += [("message_delta", {"type": "message_delta", "delta": {"stop_reason": "end_turn"}, "usage": {"output_tokens": 21}}),
                       ("message_stop", {"type": "message_stop"})]
            frames = ["event: %s\ndata: %s\n\n" % (e, json.dumps(d)) for e, d in events]
        elif path == "/v1/chat/completions":
            if self.headers.get("Authorization") != "Bearer test-key-openai":
                return self.fail("bad Authorization")
            if body["messages"][0]["role"] != "system" or "max_completion_tokens" not in body:
                return self.fail("bad body")
            frames = ["data: %s\n\n" % json.dumps({"choices": [{"delta": {"content": t}}]}) for t in TEXT]
            frames += ["data: %s\n\n" % json.dumps({"choices": [{"delta": {}, "finish_reason": "stop"}]}),
                       "data: %s\n\n" % json.dumps({"choices": [], "usage": {"completion_tokens": 22}}), "data: [DONE]\n\n"]
        elif path.startswith("/v1beta/models/") and path.endswith(":streamGenerateContent?alt=sse"):
            if self.headers.get("x-goog-api-key") != "test-key-gemini":
                return self.fail("bad x-goog-api-key")
            frames = ["data: %s\n\n" % json.dumps({"candidates": [{"content": {"parts": [{"text": t}]}}]}) for t in TEXT]
            frames += ["data: %s\n\n" % json.dumps({"candidates": [{"content": {"parts": []}, "finishReason": "STOP"}],
                                                    "usageMetadata": {"candidatesTokenCount": 23}})]
        else:
            return self.fail("unknown path %s" % path)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        for f in frames:
            b = f.encode()
            self.wfile.write(b"%x\r\n%s\r\n" % (len(b), b))
            self.wfile.flush()
            time.sleep(0.05)
        self.wfile.write(b"0\r\n\r\n")


if __name__ == "__main__":
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler)
    print("ready", flush=True)
    srv.serve_forever()

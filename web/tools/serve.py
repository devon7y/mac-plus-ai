#!/usr/bin/env python3
"""Serve web/dist (or another folder) locally the way Cloudflare Pages will: the _headers file's
cross-origin isolation headers on every response, and the right MIME types.

    python3 -I web/tools/serve.py [--dir web/dist] [--port 8642]
"""
import argparse
import functools
import http.server
import os

HEADERS = {
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "require-corp",
    "Cross-Origin-Resource-Policy": "same-origin",
    "X-Content-Type-Options": "nosniff",
}


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm", ".js": "text/javascript", ".mjs": "text/javascript",
        ".glb": "model/gltf-binary", ".gz": "application/gzip", ".svg": "image/svg+xml",
        ".rom": "application/octet-stream", ".txt": "text/plain; charset=utf-8",
        ".html": "text/html; charset=utf-8", ".css": "text/css; charset=utf-8",
    }

    def end_headers(self):
        for k, v in HEADERS.items():
            self.send_header(k, v)
        if "/assets/" in self.path:
            self.send_header("Cache-Control", "public, max-age=31536000, immutable")
        else:
            self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def log_message(self, fmt, *args):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "dist"))
    ap.add_argument("--port", type=int, default=8642, help="0 = any free port")
    a = ap.parse_args()
    handler = functools.partial(Handler, directory=os.path.abspath(a.dir))
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), handler)
    print(f"serving {a.dir} on http://127.0.0.1:{srv.server_address[1]}/", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()

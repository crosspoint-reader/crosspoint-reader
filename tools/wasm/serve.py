#!/usr/bin/env python3
"""Serve the generated preview on loopback without caching during development."""

import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class PreviewHandler(SimpleHTTPRequestHandler):
    extensions_map = {**SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm"}

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        super().end_headers()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8099)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2] / "build/preview"
    if not (root / "index.html").exists():
        parser.error("Build the preview first: python3 tools/wasm/build.py")
    with ThreadingHTTPServer(("127.0.0.1", args.port), partial(PreviewHandler, directory=str(root))) as server:
        print(f"CrossPoint preview: http://127.0.0.1:{args.port}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Preview the web interface's pages straight from src/network/html, without a firmware build.

Serves every page the reader serves, with #include directives and the %%CROSSPOINT%% /
%%VERSION%% placeholders resolved as scripts/build_html.py does (but not minified), so an edit
shows on the next browser refresh. Everything else (the /api/* endpoints, /upload, /download, ...)
goes to one of:

  --device http://192.168.0.42   a reader in File Transfer: requests are passed through, so the
                                 pages work on real data and really change it
  --mock DIR                     JSON files: GET /api/settings answers DIR/api/settings.json, and
                                 GET /api/files?path=/Books DIR/api/files@path=%2FBooks.json when
                                 that exists; other methods answer {"ok": true} and change nothing

With --device, --record DIR also saves every JSON answer to a GET under DIR, in the layout
--mock reads, so a session with the reader can be replayed without it.

  python3 scripts/web_preview.py --device http://192.168.0.42
  python3 scripts/web_preview.py --mock web-mock            # then open http://localhost:8000/settings

The WebSocket upload (port 81) is not passed through; the File Manager falls back to /upload.

For headless screenshots, ?shot=<ms> on a page holds its load event that long (a hidden image the
server answers late), so the page's own requests finish before e.g. `firefox --screenshot` fires,
and ?run=<code> runs a snippet shortly after the page is parsed (?run=openUploadModal() to shoot a
dialog); such a snippet can fetch any file of the checkout from /__repo/<path>.
"""

import argparse
import http.client
import json
import os
import re
import sys
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HTML = os.path.join(ROOT, "src", "network", "html")

# Page routes, as registered in CrossPointWebServer::begin().
PAGES = {
    "/": "WelcomePage.html",
    "/setup": "WebSetupPage.html",
    "/files": "FilesPage.html",
    "/settings": "SettingsPage.html",
    "/fonts": "FontsPage.html",
    "/stats": "StatsPage.html",
    "/systeminfo": "HomePage.html",
    "/plugins": "PluginsPage.html",
    "/dictionary": "DictionaryPage.html",
    "/diagnostics": "DiagnosticsPage.html",
}
# Where /js/<name> looks after src/network/html/js: the files build_html.py takes from elsewhere
# (EXTERNAL_ASSETS there), here served straight from their folders.
EXTRA_JS_DIRS = [os.path.join(ROOT, d) for d in ("tools/ftwasm/dist", "tools/ftwasm/js", "tools/ftwasm/dist/licenses")]
CONTENT_TYPES = {".wasm": "application/wasm", ".mjs": "application/javascript", ".js": "application/javascript",
                 ".txt": "text/plain; charset=utf-8"}
INCLUDE_RE = re.compile(r'<!--\s*#include\s+file="([^"]+)"\s*-->')
# A 1x1 transparent GIF, the answer to the ?shot= delay image.
PIXEL = bytes.fromhex("47494638396101000100800000000000ffffff21f90401000000002c00000000010001000002024401003b")
HOP_BY_HOP = {"connection", "keep-alive", "transfer-encoding", "te", "trailer", "upgrade", "proxy-authenticate",
              "proxy-authorization", "content-length"}


def firmware_name_and_version() -> tuple:
    name, version = "Witch Reader", "dev"
    section = None
    with open(os.path.join(ROOT, "platformio.ini"), encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line.startswith("["):
                section = line
            elif section == "[crosspoint]" and "=" in line:
                key, value = (part.strip() for part in line.split("=", 1))
                if key == "name":
                    name = value.strip('"')
                elif key == "version":
                    version = value
    return name, version + "-preview"


NAME, VERSION = firmware_name_and_version()


def render_page(file_name: str) -> bytes:
    path = os.path.join(HTML, file_name)
    with open(path, encoding="utf-8") as f:
        html = f.read()
    for _ in range(8):
        html, count = INCLUDE_RE.subn(lambda m: open(os.path.join(HTML, m.group(1)), encoding="utf-8").read(), html)
        if not count:
            break
    return html.replace("%%CROSSPOINT%%", NAME).replace("%%VERSION%%", VERSION).encode("utf-8")


def before_last_body_end(page: bytes, insert: bytes) -> bytes:
    """Inserts before the page's closing body tag: the last one, since a page's scripts may hold the
    same text in a string (the File Manager's EPUB templates do)."""
    head, sep, tail = page.rpartition(b"</body>")
    return head + insert + sep + tail if sep else page + insert


class Handler(BaseHTTPRequestHandler):
    device = None  # urllib.parse.SplitResult of --device
    mock_dir = None
    record_dir = None

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.command, fmt % args))

    def send_body(self, status: int, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length else b""

    def handle_any(self) -> None:
        url = urllib.parse.urlsplit(self.path)
        path = url.path
        query = urllib.parse.parse_qs(url.query)
        if self.command == "GET" and path in PAGES:
            page = render_page(PAGES[path])
            if "run" in query:
                # ?run=openUploadModal() runs that shortly after the page is parsed, e.g. to shoot a
                # dialog; with ?shot=, before the held load event that takes the screenshot.
                code = json.dumps(query["run"][0])
                script = f"<script>addEventListener('DOMContentLoaded', () => setTimeout(() => eval({code}), 500));</script>"
                page = before_last_body_end(page, script.encode())
            if "shot" in query:
                img = f'<img src="/__delay?ms={int(query["shot"][0])}" alt="" style="display:none">'.encode()
                page = before_last_body_end(page, img)
            return self.send_body(200, page, "text/html; charset=utf-8")
        if self.command == "GET" and path == "/__delay":
            time.sleep(int(query.get("ms", ["1000"])[0]) / 1000)
            return self.send_body(200, PIXEL, "image/gif")
        if self.command == "GET" and path.startswith("/js/"):
            name = os.path.basename(path)
            for folder in [os.path.join(HTML, "js")] + EXTRA_JS_DIRS:
                local = os.path.join(folder, name)
                if os.path.isfile(local):
                    with open(local, "rb") as f:
                        return self.send_body(200, f.read(), CONTENT_TYPES.get(os.path.splitext(name)[1], "application/octet-stream"))
        if self.command == "GET" and path.startswith("/__repo/"):
            # Any file of the checkout, for a ?run= snippet that needs a real input (a font, a book).
            local = os.path.normpath(os.path.join(ROOT, urllib.parse.unquote(path[len("/__repo/"):])))
            if local.startswith(ROOT + os.sep) and os.path.isfile(local):
                with open(local, "rb") as f:
                    return self.send_body(200, f.read(), "application/octet-stream")
        if self.device:
            return self.proxy()
        if self.mock_dir:
            return self.mock(path)
        self.send_body(502, b'{"error":"no --device or --mock given"}', "application/json")

    def proxy(self) -> None:
        body = self.read_body()
        headers = {k: v for k, v in self.headers.items() if k.lower() not in HOP_BY_HOP and k.lower() != "host"}
        conn = http.client.HTTPConnection(self.device.hostname, self.device.port or 80, timeout=120)
        try:
            conn.request(self.command, self.path, body=body or None, headers=headers)
            resp = conn.getresponse()
            data = resp.read()
        except OSError as e:
            return self.send_body(502, json.dumps({"error": f"reader unreachable: {e}"}).encode(), "application/json")
        finally:
            conn.close()
        self.send_response(resp.status)
        for key, value in resp.getheaders():
            if key.lower() not in HOP_BY_HOP:
                self.send_header(key, value)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        content_type = resp.getheader("Content-Type", "")
        if self.record_dir and self.command == "GET" and resp.status == 200 and "json" in content_type:
            out = os.path.join(self.record_dir, urllib.parse.urlsplit(self.path).path.lstrip("/") + ".json")
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, "wb") as f:
                f.write(data)

    def mock(self, path: str) -> None:
        if self.command != "GET":
            body = self.read_body()
            sys.stderr.write(f"  mock {self.command} {path}: {body[:200]!r}\n")
            return self.send_body(200, b'{"ok":true}', "application/json")
        # A file for this exact query first (api/files@path=%2FBooks.json), then one for any query.
        query = urllib.parse.urlsplit(self.path).query
        candidates = [path.lstrip("/") + ".json"]
        if query:
            candidates.insert(0, path.lstrip("/") + "@" + urllib.parse.quote(urllib.parse.unquote(query), safe="=&") + ".json")
        local = next((os.path.join(self.mock_dir, c) for c in candidates
                      if os.path.isfile(os.path.join(self.mock_dir, c))), "")
        if local:
            with open(local, "rb") as f:
                return self.send_body(200, f.read(), "application/json")
        self.send_body(404, b'{"error":"no mock for this path"}', "application/json")

    do_GET = do_POST = do_PUT = do_DELETE = handle_any


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device", help="base URL of a reader in File Transfer, e.g. http://192.168.0.42")
    parser.add_argument("--mock", help="folder of JSON answers, e.g. <folder>/api/settings.json")
    parser.add_argument("--record", help="with --device: save JSON answers to GET requests under this folder")
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()
    if bool(args.device) == bool(args.mock):
        parser.error("give exactly one of --device and --mock")
    if args.device:
        Handler.device = urllib.parse.urlsplit(args.device if "://" in args.device else "http://" + args.device)
    Handler.mock_dir = args.mock
    Handler.record_dir = args.record
    print(f"Previewing {NAME} pages on http://localhost:{args.port}/ ("
          f"{'reader ' + args.device if args.device else 'mock ' + args.mock})")
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()

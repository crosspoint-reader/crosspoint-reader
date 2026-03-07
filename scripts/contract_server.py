#!/usr/bin/env python3
"""
CrossPoint firmware contract server — faithful Python implementation of the HTTP API.

Exposes /_test/* endpoints for test lifecycle management so that Android
WifiTransportTest can drive this server the same way it drives FakeDeviceServer.

Usage:
    python3 scripts/contract_server.py [--port PORT]   (default: 8765)

Firmware endpoints implemented:
    GET  /api/status
    GET  /api/plugins
    GET  /api/files?path=X
    GET  /download?path=X
    POST /mkdir          (form: name, path)
    POST /rename         (form: path, name)
    POST /move           (form: path, dest)
    POST /delete         (form: paths JSON array)
    GET  /api/settings
    GET  /api/settings/raw
    POST /api/settings   (JSON body)
    GET  /api/recent
    GET  /api/cover?path=X
    GET  /api/sleep-images
    GET  /api/sleep-cover
    POST /api/sleep-cover/pin  (JSON: {path} or {bookPath})
    GET  /api/wifi/scan
    POST /api/wifi/connect     (JSON: {ssid, password})
    POST /api/wifi/forget      (JSON: {ssid})
    POST /api/open-book        (JSON: {path})
    POST /api/remote/button    (JSON: {button})
    POST /api/ota/check
    GET  /api/ota/check
    POST /api/todo/entry       (form: type, text)
    POST /api/user-fonts/upload
    POST /api/user-fonts/rescan

Test lifecycle endpoints (not present on real firmware):
    POST /_test/reset      clear all state and recorded mutations
    POST /_test/seed       merge JSON body into server state
    POST /_test/error_path add a path to the 500-error set
    GET  /_test/mutations  return recorded mutations as JSON
    GET  /_test/state      return full server state as JSON
    GET  /_test/ping       health check → "pong"
"""

import argparse
import base64
import json
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer


# ── Default state factories ──────────────────────────────────────────────────

def _default_status():
    return {
        "version": "1.0.0",
        "protocolVersion": 1,
        "wifiStatus": "connected",
        "ip": "192.168.4.1",
        "mode": "STA",
        "rssi": -50,
        "freeHeap": 100000,
        "uptime": 1000,
        "openBook": "",
        "otaSelectedBundle": "",
        "otaInstalledBundle": "",
    }


def _default_plugins():
    return {
        "web_wifi_setup": False,
        "ota_updates": False,
        "remote_open_book": True,
        "remote_page_turn": True,
        "user_fonts": False,
        "todo_planner": False,
    }


def _default_ota():
    return {
        "status": "idle",
        "available": False,
        "latestVersion": "",
        "latest_version": "",
        "errorCode": 0,
        "error_code": 0,
        "message": "",
    }


def _default_state():
    return {
        # path → list[{name, isDirectory, size, modified, isEpub}]
        "files": {},
        # list[{key, type, value, options, min, max, step, category, hasValue}]
        "settings": [],
        # list[{path, title, author, last_position, last_opened}]
        "recentBooks": [],
        "status": _default_status(),
        "plugins": _default_plugins(),
        # list[{path, name}]
        "sleepImages": [],
        "pinnedSleepCover": {"path": "", "name": ""},
        # list[{ssid, rssi, secured, connected}]
        "wifiNetworks": [],
        "otaStatus": _default_ota(),
        # path → base64-encoded bytes (for cover/download endpoints)
        "binaryFiles": {},
        # paths that respond with HTTP 500 (simulated errors)
        "errorPaths": [],
    }


def _default_mutations():
    return {
        "settingsMutations": [],   # list[dict]
        "pinRequests": [],         # list[str]  ("" means clear)
        "todoEntries": [],         # list[[type, text]]
        "wifiConnects": [],        # list[[ssid, password]]
        "wifiForgets": [],         # list[str]
        "openBookRequests": [],    # list[str]
        "remoteButtonRequests": [],# list[str]
        "deletedPaths": [],        # list[list[str]]
        "renames": [],             # list[[from_path, new_name]]
        "moves": [],               # list[[from_path, dest_path]]
        "createdDirs": [],         # list[[name, parent_path]]
    }


# ── Global mutable server state (lock-protected) ─────────────────────────────

_lock = threading.Lock()
_state = _default_state()
_mutations = _default_mutations()


# ── HTTP handler ─────────────────────────────────────────────────────────────

class ContractHandler(BaseHTTPRequestHandler):

    def log_message(self, fmt, *args):  # suppress per-request logging
        pass

    # ── Request parsing helpers ──────────────────────────────────────────────

    def _read_body(self):
        length = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(length) if length else b""

    def _parse_json(self, raw: bytes) -> dict:
        try:
            return json.loads(raw or b"{}")
        except json.JSONDecodeError:
            return {}

    def _parse_form(self, raw: bytes) -> dict:
        decoded = raw.decode("utf-8", errors="replace")
        parsed = urllib.parse.parse_qs(decoded, keep_blank_values=True)
        return {k: v[0] for k, v in parsed.items()}

    def _path_and_query(self):
        parsed = urllib.parse.urlparse(self.path)
        return parsed.path, parsed.query

    def _query_param(self, query: str, key: str) -> str:
        params = urllib.parse.parse_qs(query)
        values = params.get(key, [])
        return urllib.parse.unquote(values[0]) if values else ""

    # ── Response helpers ─────────────────────────────────────────────────────

    def _send_raw(self, code: int, content_type: str, body: bytes):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _json_response(self, obj, code: int = 200):
        self._send_raw(code, "application/json", json.dumps(obj, separators=(",", ":")).encode())

    def _text(self, body: str = "OK", code: int = 200):
        self._send_raw(code, "text/plain", body.encode())

    def _not_found(self):
        self._text("Not found", 404)

    # ── GET dispatch ─────────────────────────────────────────────────────────

    def do_GET(self):
        base, query = self._path_and_query()
        body = self._read_body()  # usually empty for GET

        with _lock:
            if base in _state["errorPaths"]:
                self._text("Simulated server error", 500)
                return

            if base == "/api/status":
                self._json_response(_state["status"])

            elif base == "/api/plugins":
                self._json_response(_state["plugins"])

            elif base == "/api/files":
                path = self._query_param(query, "path") or "/"
                entries = _state["files"].get(path, [])
                self._json_response(entries)

            elif base == "/download":
                path = self._query_param(query, "path")
                b64 = _state["binaryFiles"].get(path, "")
                data = base64.b64decode(b64) if b64 else b""
                self._send_raw(200, "application/octet-stream", data)

            elif base == "/api/settings":
                self._json_response(_state["settings"])

            elif base == "/api/settings/raw":
                self._json_response(
                    {entry.get("key", ""): entry.get("value") for entry in _state["settings"]}
                )

            elif base == "/api/recent":
                self._json_response(_state["recentBooks"])

            elif base == "/api/cover":
                path = self._query_param(query, "path")
                b64 = _state["binaryFiles"].get(path, "")
                if not b64:
                    self._text("No cover available", 404)
                    return
                self._send_raw(200, "image/bmp", base64.b64decode(b64))

            elif base == "/api/sleep-images":
                self._json_response(_state["sleepImages"])

            elif base == "/api/sleep-cover":
                self._json_response(_state["pinnedSleepCover"])

            elif base == "/api/wifi/scan":
                # Mirror firmware: return both secured and encrypted fields
                networks = []
                for n in _state["wifiNetworks"]:
                    entry = dict(n)
                    entry["encrypted"] = entry.get("secured", False)
                    entry.setdefault("saved", False)
                    networks.append(entry)
                self._json_response(networks)

            elif base == "/api/ota/check":
                self._json_response(_state["otaStatus"])

            elif base == "/_test/ping":
                self._text("pong")

            elif base == "/_test/mutations":
                self._json_response(_mutations)

            elif base == "/_test/state":
                self._json_response(_state)

            else:
                self._not_found()

    # ── POST dispatch ────────────────────────────────────────────────────────

    def do_POST(self):
        base, _ = self._path_and_query()
        raw = self._read_body()

        with _lock:
            if base in _state["errorPaths"]:
                self._text("Simulated server error", 500)
                return

            if base == "/api/settings":
                body = self._parse_json(raw)
                _mutations["settingsMutations"].append(body)
                self._text(f"Applied {len(body)} setting(s)")

            elif base == "/mkdir":
                form = self._parse_form(raw)
                name = form.get("name", "")
                path = form.get("path", "/")
                _mutations["createdDirs"].append([name, path])
                self._text(f"Folder created: {name}")

            elif base == "/rename":
                form = self._parse_form(raw)
                path = urllib.parse.unquote(form.get("path", ""))
                name = form.get("name", "")
                _mutations["renames"].append([path, name])
                self._text("Renamed")

            elif base == "/move":
                form = self._parse_form(raw)
                path = urllib.parse.unquote(form.get("path", ""))
                dest = urllib.parse.unquote(form.get("dest", ""))
                _mutations["moves"].append([path, dest])
                self._text("Moved")

            elif base == "/delete":
                form = self._parse_form(raw)
                try:
                    paths = json.loads(form.get("paths", "[]"))
                except json.JSONDecodeError:
                    paths = []
                _mutations["deletedPaths"].append(paths)
                self._text(f"Deleted {len(paths)} item(s)")

            elif base == "/api/todo/entry":
                form = self._parse_form(raw)
                _mutations["todoEntries"].append(
                    [form.get("type", ""), form.get("text", "")]
                )
                self._text("Added")

            elif base == "/api/sleep-cover/pin":
                self._handle_sleep_pin(raw)

            elif base == "/api/open-book":
                body = self._parse_json(raw)
                path = body.get("path", "")
                _mutations["openBookRequests"].append(path)
                _state["status"]["openBook"] = path
                self._json_response({"status": "opening"}, 202)

            elif base == "/api/remote/button":
                body = self._parse_json(raw)
                _mutations["remoteButtonRequests"].append(body.get("button", ""))
                self._json_response({"status": "ok"}, 202)

            elif base == "/api/wifi/connect":
                body = self._parse_json(raw)
                _mutations["wifiConnects"].append(
                    [body.get("ssid", ""), body.get("password", "")]
                )
                self._text("WiFi credentials saved")

            elif base == "/api/wifi/forget":
                body = self._parse_json(raw)
                ssid = body.get("ssid", "")
                _mutations["wifiForgets"].append(ssid)
                self._text("WiFi credentials removed")

            elif base == "/api/ota/check":
                # POST triggers a check; respond 202 immediately
                self._json_response({"status": "checking"}, 202)

            elif base in ("/api/user-fonts/upload", "/api/user-fonts/rescan"):
                self._text()

            elif base == "/_test/reset":
                _state.clear()
                _state.update(_default_state())
                _mutations.clear()
                _mutations.update(_default_mutations())
                self._text("Reset")

            elif base == "/_test/seed":
                body = self._parse_json(raw)
                for key, value in body.items():
                    if key in _state:
                        _state[key] = value
                self._text("Seeded")

            elif base == "/_test/error_path":
                body = self._parse_json(raw)
                path = body.get("path", "")
                if path and path not in _state["errorPaths"]:
                    _state["errorPaths"].append(path)
                self._text("Error path added")

            else:
                self._not_found()

    # ── Sleep-cover pin handler (Mode A: path, Mode B: bookPath) ────────────

    def _handle_sleep_pin(self, raw: bytes):
        body = self._parse_json(raw)
        book_path = body.get("bookPath")

        if book_path is not None:
            # Mode B: pin a book cover.
            # Simplified: always succeeds and records the bookPath.
            # Real firmware resolves the cover BMP and copies it; here we
            # simulate the successful outcome so Android client tests pass.
            dest = "/sleep/.pinned-cover.bmp"
            _mutations["pinRequests"].append(book_path)
            _state["pinnedSleepCover"] = {"path": dest, "name": ".pinned-cover.bmp"}
            self._json_response({"pinnedPath": dest})
            return

        # Mode A: pin by path (empty string → clear)
        pin_path = body.get("path", "")
        _mutations["pinRequests"].append(pin_path)

        if not pin_path:
            _state["pinnedSleepCover"] = {"path": "", "name": ""}
            # Firmware returns plain text "Cleared" for clear operations
            self._text("Cleared")
            return

        # Verify path is in sleep images (if seeded); fall back to synthetic entry
        match = next(
            (img for img in _state["sleepImages"] if img.get("path") == pin_path),
            None,
        )
        if match:
            _state["pinnedSleepCover"] = dict(match)
        else:
            name = pin_path.rsplit("/", 1)[-1]
            _state["pinnedSleepCover"] = {"path": pin_path, "name": name}

        # Firmware returns {"pinnedPath": "..."} JSON on success
        self._json_response({"pinnedPath": pin_path})


# ── Entry point ──────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="CrossPoint firmware contract server")
    parser.add_argument("--port", type=int, default=8765, help="Port to listen on")
    parser.add_argument("--host", default="127.0.0.1", help="Host to bind to")
    args = parser.parse_args()

    server = HTTPServer((args.host, args.port), ContractHandler)
    print(f"Contract server listening on {args.host}:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.", file=sys.stderr)


if __name__ == "__main__":
    main()

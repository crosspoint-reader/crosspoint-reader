"""Local HTTP API for the debugging monitor (no serial access in handlers)."""

import json
import re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

from monitor_session import SessionError


def create_server(session, port=8765):
    class Handler(BaseHTTPRequestHandler):
        def setup(self):
            super().setup()
            self.connection.settimeout(35)

        def log_message(self, *_args):
            pass

        def _reply(self, status, data, content_type="application/json"):
            payload = (
                json.dumps(data).encode()
                if content_type == "application/json"
                else data
            )
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(payload)

        def _dispatch(self, write=False):
            try:
                # Browser pages are not API clients. No CORS or form-based writes.
                if self.headers.get("Origin"):
                    raise SessionError("Browser-origin requests are not supported", 403)
                url = urlsplit(self.path)
                if write:
                    if (
                        self.headers.get("Content-Type", "").split(";")[0]
                        != "application/json"
                    ):
                        raise SessionError("Content-Type must be application/json", 415)
                    length = int(self.headers.get("Content-Length", "0"))
                    if not 0 < length <= 4096:
                        raise SessionError("JSON body must be 1–4096 bytes", 400)
                    body = json.loads(self.rfile.read(length))
                    if not isinstance(body, dict):
                        raise SessionError("Expected a JSON object", 400)
                    token = self.headers.get("X-Control-Token")
                    if url.path == "/control/claim":
                        result = session.claim(token, body.get("ttl", 60))
                    elif url.path == "/control/release":
                        result = session.unclaim(token)
                    elif url.path == "/command":
                        result = session.request("command", body.get("command"), token)
                    elif url.path == "/screenshot":
                        result = session.request("command", "SCREENSHOT", token)
                    elif url.path in ("/serial/release", "/serial/reconnect"):
                        result = session.request(
                            url.path.rsplit("/", 1)[1], token=token
                        )
                    else:
                        raise SessionError("Unknown endpoint", 404)
                elif url.path == "/status":
                    result = session.status()
                elif url.path == "/events":
                    args = parse_qs(url.query)
                    after = int(args.get("after", ["0"])[0])
                    wait = float(args.get("wait", ["0"])[0])
                    if after < 0 or not 0 <= wait <= 30:
                        raise SessionError(
                            "after must be nonnegative; wait must be 0–30 seconds", 400
                        )
                    result = session.read_events(after, wait)
                elif re.fullmatch(r"/screenshots/screenshot-\d+\.(raw|pbm)", url.path):
                    path = session.output_dir / url.path.rsplit("/", 1)[1]
                    if not path.is_file():
                        raise SessionError("Screenshot not found", 404)
                    self._reply(200, path.read_bytes(), "application/octet-stream")
                    return
                else:
                    raise SessionError("Unknown endpoint", 404)
                self._reply(200, result)
            except SessionError as exc:
                self._reply(exc.status, {"error": str(exc)})
            except (ValueError, TypeError, UnicodeError):
                self._reply(400, {"error": "Invalid request parameters or JSON"})
            except (BrokenPipeError, ConnectionResetError, TimeoutError):
                pass

        def do_GET(self):
            self._dispatch()

        def do_POST(self):
            self._dispatch(write=True)

    return ThreadingHTTPServer(("127.0.0.1", port), Handler)

#!/usr/bin/env python3
"""
CrossPoint content feed server.

Serves EPUBs, BMPs, news items, and firmware as an RSS feed that the
CrossPoint reader can sync over WiFi. All items are sorted globally
newest-first — required by the reader's timestamp-based dedup logic.

Usage:
    python3 feed_server.py [--port 8090] [--content-dir ./content] [--host 0.0.0.0]

Feed available at:
    http://<host>:<port>/feed.xml

Files are served at:
    http://<host>:<port>/content/<dir>/<filename>

See docs/rss-content-feeds.md for the full content model and integration guide.
"""

import argparse
import os
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path
from datetime import datetime, timezone
import json

# --- Defaults (override via CLI args or environment) ---
DEFAULT_PORT = int(os.environ.get("FEED_PORT", 8090))
DEFAULT_CONTENT_DIR = Path(os.environ.get("FEED_CONTENT_DIR", Path(__file__).parent / "content"))
DEFAULT_HOST = os.environ.get("FEED_HOST", "0.0.0.0")
DEFAULT_FEED_URL = os.environ.get("FEED_URL", "")  # auto-detected if empty
DEFAULT_FEED_TITLE = os.environ.get("FEED_TITLE", "CrossPoint Content Feed")

# --- Content rules: map subdirectory -> feed item type + reader destination path ---
CONTENT_RULES = [
    {"dir": "books/chip",   "ext": ".epub", "type": "file",     "path": "/Books/chip/"},
    {"dir": "books/erotic", "ext": ".epub", "type": "file",     "path": "/Books/erotic/"},
    {"dir": "thought",      "ext": ".epub", "type": "file",     "path": "/Thought/"},
    {"dir": "trips",        "ext": ".epub", "type": "file",     "path": "/trips/"},
    {"dir": "sleep",        "ext": ".bmp",  "type": "image",    "path": "/sleep/"},
    {"dir": "news",         "ext": ".json", "type": "news",     "path": None},
    {"dir": "firmware",     "ext": ".bin",  "type": "firmware", "path": None},
]


def fmt_date(dt: datetime) -> str:
    """RFC 2822 format matching parseRfc2822() in RssFeedSync.cpp."""
    return dt.strftime("%a, %d %b %Y %H:%M:%S +0000")


def file_guid(f: Path) -> str:
    return f"{f.stem}-{int(f.stat().st_mtime)}"


def escape_xml(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def build_feed(content_dir: Path, feed_url: str, feed_title: str) -> str:
    """Build the full RSS feed XML, all items sorted newest-first."""
    dated = []

    for rule in CONTENT_RULES:
        content_path = content_dir / rule["dir"]
        if not content_path.exists():
            continue

        if rule["type"] == "news":
            for f in sorted(content_path.glob("*.json")):
                try:
                    data = json.loads(f.read_text())
                except Exception:
                    continue
                mtime = f.stat().st_mtime
                dt = datetime.fromtimestamp(mtime, tz=timezone.utc)
                pub_date = data.get("date", fmt_date(dt))
                title = escape_xml(data.get("title", f.stem))
                body = data.get("body", "")
                dated.append((mtime, f"""
    <item>
      <title>{title}</title>
      <crosspoint:type>news</crosspoint:type>
      <description><![CDATA[{body}]]></description>
      <pubDate>{pub_date}</pubDate>
      <guid>{f.stem}</guid>
    </item>"""))

        elif rule["type"] == "firmware":
            version_file = content_path / ".version"
            version = version_file.read_text().strip() if version_file.exists() else None
            bins = sorted(content_path.glob("*.bin"), key=lambda x: x.stat().st_mtime, reverse=True)[:1]
            for f in bins:
                mtime = f.stat().st_mtime
                dt = datetime.fromtimestamp(mtime, tz=timezone.utc)
                url = f"{feed_url}/content/firmware/{f.name}"
                guid = f"firmware-{version}" if version else file_guid(f)
                ver_label = version or f.stem
                dated.append((mtime, f"""
    <item>
      <title>CrossPoint Firmware {escape_xml(ver_label)}</title>
      <crosspoint:type>firmware</crosspoint:type>
      <enclosure url="{url}" type="application/octet-stream" length="{f.stat().st_size}"/>
      <pubDate>{fmt_date(dt)}</pubDate>
      <guid>{guid}</guid>
    </item>"""))

        else:
            mime = "application/epub+zip" if rule["ext"] == ".epub" else "image/bmp"
            subdir = rule["dir"]
            dest_path = rule["path"]
            for f in sorted(content_path.glob(f"*{rule['ext']}")):
                mtime = f.stat().st_mtime
                dt = datetime.fromtimestamp(mtime, tz=timezone.utc)
                url = f"{feed_url}/content/{subdir}/{f.name}"
                title = escape_xml(f.stem.replace("-", " ").replace("_", " ").title())
                dated.append((mtime, f"""
    <item>
      <title>{title}</title>
      <crosspoint:type>{rule['type']}</crosspoint:type>
      <crosspoint:path>{dest_path}</crosspoint:path>
      <enclosure url="{escape_xml(url)}" type="{mime}" length="{f.stat().st_size}"/>
      <pubDate>{fmt_date(dt)}</pubDate>
      <guid>{escape_xml(file_guid(f))}</guid>
    </item>"""))

    # Firmware items always first, then everything else newest-first
    firmware_items = [(t, xml) for t, xml in dated if "<crosspoint:type>firmware</crosspoint:type>" in xml]
    other_items = [(t, xml) for t, xml in dated if "<crosspoint:type>firmware</crosspoint:type>" not in xml]
    other_items.sort(key=lambda x: x[0], reverse=True)
    dated = firmware_items + other_items
    items_xml = "\n".join(xml for _, xml in dated)

    # Total downloadable item count (all items with enclosures: files, images, firmware)
    item_count = sum(1 for _, xml in dated if "<enclosure" in xml)

    now = fmt_date(datetime.now(timezone.utc))
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<rss version="2.0" xmlns:crosspoint="https://crosspoint.example/ns">
  <channel>
    <title>{escape_xml(feed_title)}</title>
    <link>{feed_url}/feed.xml</link>
    <description>CrossPoint content feed</description>
    <lastBuildDate>{now}</lastBuildDate>
    <crosspoint:itemCount>{item_count}</crosspoint:itemCount>
{items_xml}
  </channel>
</rss>"""


def make_handler(content_dir, feed_url, feed_title, access_log, reader_feed_file=None):
    class FeedHandler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == "/reader-feed.xml" and reader_feed_file:
                # Proxy an external news/reader feed so the ESP32 can fetch it
                # over HTTP/1.1 (python3 -m http.server only does HTTP/1.0)
                p = Path(reader_feed_file)
                if p.exists():
                    body = p.read_bytes()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/rss+xml; charset=utf-8")
                    self.send_header("Content-Length", len(body))
                    self.end_headers()
                    self.wfile.write(body)
                else:
                    self.send_response(404)
                    self.end_headers()
                return

            elif self.path == "/feed.xml":
                body = build_feed(content_dir, feed_url, feed_title).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/rss+xml; charset=utf-8")
                self.send_header("Content-Length", len(body))
                self.end_headers()
                self.wfile.write(body)

            elif self.path.startswith("/content/"):
                rel = self.path[len("/content/"):]
                # Security: block path traversal
                file_path = (content_dir / rel).resolve()
                if not str(file_path).startswith(str(content_dir.resolve())):
                    self.send_response(403)
                    self.end_headers()
                    return
                if file_path.exists() and file_path.is_file():
                    body = file_path.read_bytes()
                    self.send_response(200)
                    self.send_header("Content-Length", len(body))
                    self.end_headers()
                    self.wfile.write(body)
                else:
                    self.send_response(404)
                    self.end_headers()

            else:
                self.send_response(404)
                self.end_headers()

        def log_request(self, code="-", size="-"):
            line = f"[{datetime.now().strftime('%H:%M:%S')}] {self.address_string()} {self.command} {self.path} -> {code}"
            print(line)
            if access_log:
                with open(access_log, "a") as lf:
                    lf.write(line + "\n")

        def log_message(self, format, *args):
            pass

    return FeedHandler


def main():
    parser = argparse.ArgumentParser(description="CrossPoint RSS content feed server")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--content-dir", type=Path, default=DEFAULT_CONTENT_DIR)
    parser.add_argument("--feed-url", default=DEFAULT_FEED_URL,
                        help="Public base URL (auto-detected from --host/--port if omitted)")
    parser.add_argument("--feed-title", default=DEFAULT_FEED_TITLE)
    parser.add_argument("--access-log", default=None, help="Optional path for access log")
    parser.add_argument("--reader-feed", default=os.environ.get("CROSSPOINT_READER_FEED"),
                        help="Path to an external reader/news feed XML to proxy at /reader-feed.xml "
                             "(also settable via CROSSPOINT_READER_FEED env var)")
    args = parser.parse_args()

    content_dir = args.content_dir.resolve()
    feed_url = args.feed_url or f"http://{args.host if args.host != '0.0.0.0' else 'localhost'}:{args.port}"

    print(f"CrossPoint feed server")
    print(f"  Content dir : {content_dir}")
    print(f"  Feed URL    : {feed_url}/feed.xml")
    print(f"  Listening   : {args.host}:{args.port}")
    if args.reader_feed:
        print(f"  Reader feed : {args.reader_feed} → /reader-feed.xml")

    handler = make_handler(content_dir, feed_url, args.feed_title, args.access_log,
                           reader_feed_file=args.reader_feed)
    HTTPServer((args.host, args.port), handler).serve_forever()


if __name__ == "__main__":
    main()

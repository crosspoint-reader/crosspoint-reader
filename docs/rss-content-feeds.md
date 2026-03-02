# Building a CrossPoint Content RSS Feed

## Why RSS?

CrossPoint added RSS feed support so that an AI assistant (like [OpenClaw](https://openclaw.ai)) can deliver a fully personalized set of content to the device automatically — not just eBooks, but anything the reader can display or act on.

The feed is the bridge between your home server (or any web host) and the reader. Each time the device connects to WiFi, it checks the feed and downloads only new items. Because the feed is standard RSS 2.0 with a lightweight custom namespace, any server or script can generate it.

**What the feed can deliver:**
- **eBooks** — EPUBs, Markdown, plain text (fiction, articles, trip guides)
- **Sleep screens** — BMP wallpaper images shown when the device is idle
- **News** — structured JSON news items rendered natively
- **Firmware updates** — signed OTA firmware binaries, delivered and flashed automatically

This means OpenClaw (or any automation) can run overnight and wake up the reader with fresh content: a new short story, today's AI digest, a trip poster, and the latest firmware — all without any manual file transfer.

---

This guide explains how to build a personalized RSS content feed for CrossPoint devices — EPUBs, sleep screen images, news stories, and more. The goal is that you (or an AI assistant like Claude Code or OpenClaw) can implement a custom feed server from this document alone.

---

## How the Feed Works

CrossPoint polls a configured RSS 2.0 feed URL whenever it connects to WiFi. Items are deduplicated by `<guid>`, so each item is delivered exactly once. The feed uses a custom XML namespace to specify item type and destination path:

```xml
xmlns:crosspoint="https://crosspoint.example/ns"
```

---

## Feed Item Types

### 1. File (EPUB, Markdown, any document)

```xml
<item>
  <title>The Open Source Singularity</title>
  <crosspoint:type>file</crosspoint:type>
  <crosspoint:path>/Thought/</crosspoint:path>
  <enclosure
    url="https://example.com/articles/open-source-singularity.epub"
    type="application/epub+zip"
    length="45231"/>
  <guid>article-open-source-singularity-v1</guid>
  <pubDate>Fri, 27 Feb 2026 09:00:00 EST</pubDate>
</item>
```

- `<crosspoint:path>` — destination directory on SD card (e.g. `/Books/fiction/`, `/Thought/`, `/trips/`)
- `<enclosure url>` — direct download URL for the file
- File is saved as `<path>/<filename from URL>`

### 2. Image (BMP sleep screen)

```xml
<item>
  <title>Beardsley - Salome</title>
  <crosspoint:type>image</crosspoint:type>
  <crosspoint:path>/sleep/</crosspoint:path>
  <enclosure
    url="https://example.com/art/beardsley-salome.bmp"
    type="image/bmp"
    length="384000"/>
  <guid>art-beardsley-salome</guid>
  <pubDate>Fri, 27 Feb 2026 09:00:00 EST</pubDate>
</item>
```

- Images must be **480×800 greyscale BMP** (the device's sleep screen format)
- Pre-convert from PNG: `convert input.png -resize 480x800 -colorspace Gray output.bmp`
- Stored in `/sleep/` — shown on the device's sleep screen

### 3. News Story

```xml
<item>
  <title>Open Source Models Now Match GPT-4 on Key Benchmarks</title>
  <crosspoint:type>news</crosspoint:type>
  <description><![CDATA[
    In February 2026, three open-weight models posted benchmarks that should
    terrify every AI API company. DeepSeek V3.2, GLM-5, and Kimi K2.5 all
    scored within single digits of GPT-4 on LiveCodeBench — and they're free.

    The commoditization of AI intelligence is no longer a prediction. It's here.
  ]]></description>
  <pubDate>Fri, 27 Feb 2026 08:00:00 EST</pubDate>
  <guid>news-open-source-benchmarks-2026-02-27</guid>
</item>
```

- Stories are aggregated into `/News.md` on the SD card, newest first
- Old stories are pruned after N days (configurable in device settings, default 7)
- Format in `News.md`:
  ```markdown
  ## Headline
  *Date*
  
  Body text.
  
  ---
  ```

### 4. Firmware Update

See `development-workflow.md` for full details and security implications. Only include firmware items in feeds you control completely.

---

## Minimal Feed Server (Python)

The simplest possible feed server — a Python script that generates RSS on the fly from a content directory:

```python
#!/usr/bin/env python3
"""
CrossPoint content feed server.
Serves EPUBs, BMPs, and news items as an RSS feed.

Usage:
    python3 feed_server.py
    # Feed available at http://localhost:8090/feed.xml
"""

from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path
from datetime import datetime, timezone
import os
import json

# --- Configuration ---
CONTENT_DIR = Path("./content")   # Root directory for content files
PORT = 8090
FEED_TITLE = "My CrossPoint Feed"
FEED_URL = "http://localhost:8090"

# Map file extensions to CrossPoint types and destination paths
CONTENT_RULES = [
    {"dir": "books/fiction", "ext": ".epub", "type": "file",  "path": "/Books/fiction/"},
    {"dir": "books/other",   "ext": ".epub", "type": "file",  "path": "/Books/other/"},
    {"dir": "thought",       "ext": ".epub", "type": "file",  "path": "/Thought/"},
    {"dir": "trips",         "ext": ".epub", "type": "file",  "path": "/trips/"},
    {"dir": "sleep",         "ext": ".bmp",  "type": "image", "path": "/sleep/"},
    {"dir": "news",          "ext": ".json", "type": "news",  "path": None},
]

def format_rss_date(dt: datetime) -> str:
    return dt.strftime("%a, %d %b %Y %H:%M:%S %z")

def file_guid(filepath: Path) -> str:
    """Stable guid from file path + modification time."""
    mtime = int(filepath.stat().st_mtime)
    return f"{filepath.stem}-{mtime}"

def build_feed() -> str:
    items = []

    for rule in CONTENT_RULES:
        content_path = CONTENT_DIR / rule["dir"]
        if not content_path.exists():
            continue

        if rule["type"] == "news":
            # News items from JSON files: {"title": "...", "body": "...", "date": "..."}
            for f in sorted(content_path.glob("*.json"), key=lambda x: -x.stat().st_mtime):
                data = json.loads(f.read_text())
                pub_date = data.get("date", format_rss_date(datetime.now(timezone.utc)))
                items.append(f"""
    <item>
      <title>{data['title']}</title>
      <crosspoint:type>news</crosspoint:type>
      <description><![CDATA[{data['body']}]]></description>
      <pubDate>{pub_date}</pubDate>
      <guid>{f.stem}</guid>
    </item>""")
        else:
            # File/image items
            for f in sorted(content_path.glob(f"*{rule['ext']}"), key=lambda x: -x.stat().st_mtime):
                size = f.stat().st_size
                mtime = datetime.fromtimestamp(f.stat().st_mtime, tz=timezone.utc)
                url = f"{FEED_URL}/content/{rule['dir']}/{f.name}"
                mime = "application/epub+zip" if rule["ext"] == ".epub" else "image/bmp"
                items.append(f"""
    <item>
      <title>{f.stem.replace('-', ' ').replace('_', ' ').title()}</title>
      <crosspoint:type>{rule['type']}</crosspoint:type>
      <crosspoint:path>{rule['path']}</crosspoint:path>
      <enclosure url="{url}" type="{mime}" length="{size}"/>
      <pubDate>{format_rss_date(mtime)}</pubDate>
      <guid>{file_guid(f)}</guid>
    </item>""")

    items_xml = "\n".join(items)
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<rss version="2.0" xmlns:crosspoint="https://crosspoint.example/ns">
  <channel>
    <title>{FEED_TITLE}</title>
    <link>{FEED_URL}/feed.xml</link>
    <description>CrossPoint content feed</description>
    <lastBuildDate>{format_rss_date(datetime.now(timezone.utc))}</lastBuildDate>
{items_xml}
  </channel>
</rss>"""

class FeedHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/feed.xml":
            body = build_feed().encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/rss+xml; charset=utf-8")
            self.send_header("Content-Length", len(body))
            self.end_headers()
            self.wfile.write(body)
        elif self.path.startswith("/content/"):
            # Serve content files
            file_path = CONTENT_DIR / self.path[len("/content/"):]
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

    def log_message(self, format, *args):
        print(f"[{datetime.now().strftime('%H:%M:%S')}] {format % args}")

if __name__ == "__main__":
    print(f"CrossPoint feed server starting on port {PORT}")
    print(f"Feed URL: {FEED_URL}/feed.xml")
    print(f"Content directory: {CONTENT_DIR.resolve()}")
    HTTPServer(("", PORT), FeedHandler).serve_forever()
```

**Directory structure:**
```
content/
  books/fiction/       ← Your own stories (.epub)
  books/other/         ← Downloaded EPUBs (.epub)
  thought/            ← Thought leadership articles (.epub)
  trips/              ← Trip guides (.epub)
  sleep/              ← Sleep screen art (.bmp)
  news/               ← News items (.json)
    2026-02-27-ai-benchmarks.json
```

**News item JSON format:**
```json
{
  "title": "Open Source Models Match GPT-4",
  "body": "In February 2026, three open-weight models...",
  "date": "Fri, 27 Feb 2026 08:00:00 +0000"
}
```

---

## Personalized Feed with AI (Claude Code / OpenClaw)

The real power of this system is using an AI to generate content and push it to the feed automatically. Here's the pattern:

### Prompt to give an AI assistant

```
I want to build a personalized CrossPoint RSS content feed. 
Read docs/rss-content-feeds.md for the full spec.

Build a Python script that:
1. Fetches today's top AI news from Hacker News (filter for AI/ML posts)
2. Summarizes each story in 150 words using the Ollama API (model: your-preferred-model)
3. Saves each story as a JSON file in content/news/ with today's date in the filename
4. Fetches a random public domain artwork from Wikimedia Commons
5. Converts it to a 480×800 greyscale BMP and saves to content/sleep/
6. Runs the feed server on port 8090

Run this as a daily cron job at 6 AM.
```

### What OpenClaw does natively

OpenClaw (Chip) already runs several crons that generate content and can push it to the feed automatically:

- **Daily story generator** → generates `.md` → convert to EPUB → drop in `content/books/fiction/`
- **Daily art** (9 AM) → fetches Wikimedia art → converts to BMP → drop in `content/sleep/`
- **Daily AI digest** (6:30 AM) → summarizes AI news → save as JSON → drop in `content/news/`
- **Daily trip suggestion** (6 AM) → generates trip guide → convert to EPUB → drop in `content/trips/`

Each of these just needs to write its output to the feed server's content directory. The feed server picks it up automatically — no extra integration needed.

---

## Advanced: Dynamic Feed Generation

For more control, generate the RSS dynamically rather than from files. This lets you:
- Filter content by date range
- Personalize based on reading history
- Integrate with external APIs

```python
def build_personalized_feed(user_prefs: dict) -> str:
    """
    Generate a personalized feed based on preferences.
    
    user_prefs = {
        "genres": ["sci-fi", "literary"],
        "max_news_age_days": 3,
        "include_art": True,
        "include_firmware": False,
    }
    """
    items = []
    
    # Filter stories by genre tags
    if "genres" in user_prefs:
        items += get_stories_by_genre(user_prefs["genres"])
    
    # Fresh news only
    if user_prefs.get("max_news_age_days"):
        items += get_recent_news(days=user_prefs["max_news_age_days"])
    
    # Random sleep art
    if user_prefs.get("include_art"):
        items += get_daily_art()
    
    return render_feed(items)
```

---

## Deployment Options

| Option | Best for | Complexity |
|--------|----------|------------|
| `python3 -m http.server` | Local dev only | Trivial |
| `feed_server.py` (above) | Home server | Low |
| Nginx + static files | Production | Medium |
| Flask/FastAPI app | Dynamic/personalized | Medium |
| Hosted (GCS, S3, etc.) | Anywhere access | Low–Medium |

For home use (CrossPoint on home WiFi), the Python server running on a local machine or NAS is perfect. The device just needs to reach the server IP on your local network.

For use away from home (via Tailscale, public IP, etc.), expose the feed via HTTPS. Let's Encrypt + Caddy makes this trivial:

```
# Caddyfile
crosspoint.yourdomain.com {
    root * /var/www/crosspoint
    file_server
}
```

---

## Security Notes

- **The feed URL is your only access control.** Use a long, random path segment (like a token) to prevent casual discovery: `https://example.com/xK7mP3qR9/feed.xml`
- **Never enable firmware OTA** unless you fully control the feed server. See `development-workflow.md`.
- **Content files are served without authentication** — if you include personal content, use HTTPS and keep the URL private.
- **News.md on the device** is world-readable if anyone has the SD card. Don't put sensitive information in news stories.

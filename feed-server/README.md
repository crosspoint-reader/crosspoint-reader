# CrossPoint Feed Server

A lightweight RSS feed server that delivers EPUBs, sleep-screen BMPs, news items, and firmware OTA updates to CrossPoint readers over WiFi.

The reader fetches `feed.xml` on every WiFi connect and downloads any items it hasn't seen before, placing them in the correct directories on its SD card automatically.

## Quick Start

```bash
python3 feed_server.py --feed-url http://YOUR_SERVER_IP:8090
```

Feed is available at `http://YOUR_SERVER_IP:8090/feed.xml`.

## Content Directory Layout

```
content/
  books/chip/      → .epub  → reader:/Books/chip/
  books/erotic/    → .epub  → reader:/Books/erotic/
  thought/         → .epub  → reader:/Thought/
  trips/           → .epub  → reader:/trips/
  sleep/           → .bmp   → reader:/sleep/     (480×800 greyscale)
  news/            → .json  → displayed as news items
  firmware/        → .bin   → OTA firmware update (newest only)
```

Drop files into the appropriate subdirectory and they appear in the feed immediately. Symlinks work — point subdirs at your existing content library.

## Options

| Flag | Default | Description |
|------|---------|-------------|
| `--port` | `8090` | Port to listen on |
| `--host` | `0.0.0.0` | Bind address |
| `--content-dir` | `./content` | Root of content tree |
| `--feed-url` | auto | Public base URL for enclosure links |
| `--feed-title` | `CrossPoint Content Feed` | Feed channel title |
| `--access-log` | none | Path to write access log |

All options can also be set via environment variables: `FEED_PORT`, `FEED_HOST`, `FEED_CONTENT_DIR`, `FEED_URL`, `FEED_TITLE`.

## News Item Format

Place `.json` files in `content/news/`:

```json
{
  "title": "Article title",
  "body": "Full article text (HTML allowed).",
  "date": "Sat, 28 Feb 2026 12:00:00 +0000"
}
```

## Firmware OTA

Place the compiled `.bin` in `content/firmware/` and write the version string to `content/firmware/.version`. The server exposes only the single newest binary. The reader applies it only if **Feed Sync → Allow Firmware** is enabled in Settings.

## Running as a systemd service

```ini
[Unit]
Description=CrossPoint Feed Server
After=network.target

[Service]
ExecStart=python3 /path/to/feed_server.py --feed-url http://192.168.1.x:8090
WorkingDirectory=/path/to/feed-server
Restart=always

[Install]
WantedBy=default.target
```

## How dedup works

The reader stores a 4-byte timestamp (`feed-sync-time.bin`) of the last successful sync. On each sync it stops processing at the first item older than that timestamp — so **all items in the feed must be sorted newest-first** (the server does this automatically).

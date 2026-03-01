# CrossPoint OpenClaw — AI-Curated E-Reader

This is a fork of [CrossPoint Reader](CROSSPOINT-README.md) that extends it with **AI-driven content delivery**: an RSS feed pipeline that lets a personal AI agent (OpenClaw/Chip) automatically generate, package, and push content — news digests, EPUBs, markdown articles, sleep screen art, and firmware updates — directly to the device over WiFi.

![Home screen](docs/images/screenshots/home.png)

---

## What this fork adds

### RSS Feed Sync

The device polls an RSS feed server and automatically downloads new content to the SD card. Supported content types delivered via feed:

| Type | Extension | Notes |
|------|-----------|-------|
| EPUB books | `.epub` | Full e-reader support |
| Articles / news | `.md`, `.txt` | Rendered natively |
| Sleep screen art | `.bmp` | Displayed on sleep |
| Firmware updates | `.bin` | Flashed on next boot |

The feed server runs anywhere on your local network (or internet). A simple Python HTTP server with an RSS XML feed is all that's needed. Scripts are included in `scripts/` for pushing content.

### Danger Zone — Remote OTA & Auto-Connect

The **Danger Zone** (enabled in Settings → SYST) unlocks remote management:

- **Auto-connect on boot**: device rejoins known WiFi automatically, no manual interaction needed
- **Background web server**: file upload, EPUB management, and feed sync all accessible via browser while reading
- **Remote OTA flash**: push new firmware to the SD card and it flashes on next sleep/wake
- **QR code on boot**: device lands on the file transfer screen after auto-connect, ready to receive content

![File transfer screen with QR code](docs/images/screenshots/file_transfer.png) ![System settings — Danger Zone toggle](docs/images/screenshots/settings_syst.png)

### Web Interface

Once connected, the full web interface is accessible from any browser on the network:

![Web interface — file manager](docs/images/wifi/webserver_files.png) ![Web interface — upload](docs/images/wifi/webserver_upload.png)

### OpenClaw / Chip Integration

**OpenClaw** is a personal AI agent (Claude Sonnet, running as `claude-code`) that acts as the content brain for the device. It:

- Generates daily news digests as markdown files and pushes them via RSS
- Summarises and packages articles into readable `.md` or `.epub` files
- Creates sleep screen artwork (`.bmp`) tailored to the reader's preferences
- Monitors the feed server and triggers firmware updates autonomously
- Runs the full build → deploy → flash → verify loop without human intervention

The reader becomes a **passive delivery target**: Chip generates content on a schedule, the device syncs on boot, and new reading material appears automatically.

---

## How it works end to end

```
OpenClaw/Chip (AI agent)
    │
    │  generates content, builds firmware
    ▼
Feed Server (Python HTTP, local network)
    │  serves RSS feed + files at http://192.168.x.x:8090/feed.xml
    ▼
CrossPoint Device (ESP32-C3, Xteink X4)
    │  syncs on boot, downloads new items to SD card
    │  flashes firmware on sleep/wake if firmware.bin present
    ▼
Reader (you)
    opens new articles, books, and art — automatically delivered
```

---

## Setup

### 1. Flash CrossPoint OpenClaw firmware

Follow the standard CrossPoint flashing instructions in [CROSSPOINT-README.md](CROSSPOINT-README.md#installing), or use the web flasher at https://xteink.dve.al/ with a firmware build from this fork.

### 2. Enable Danger Zone

On the device:
1. Settings → SYST tab → toggle **Danger Zone** ON
2. Tap **Danger Zone Password** → set a password
3. Reboot

After reboot, the device auto-connects to WiFi and shows the QR code screen.

### 3. Start a feed server

```bash
# Minimal feed server (Python)
cd /path/to/your/feed/content
python3 -m http.server 8090
```

See `scripts/reader-push-news.py` for pushing news items, and `docs/rss-content-feeds.md` for the full feed format spec.

### 4. Configure the feed URL on the device

Settings → NETW → Feed URL → `http://192.168.x.x:8090/feed.xml`

---

## Screenshots

| | | |
|---|---|---|
| ![Home](docs/images/screenshots/home.png) | ![Reader](docs/images/screenshots/reader.png) | ![Browse](docs/images/screenshots/browse.png) |
| Home screen | Reading | File browser |
| ![Network](docs/images/screenshots/network_mode.png) | ![WiFi scan](docs/images/screenshots/wifi_scan.png) | ![Settings](docs/images/screenshots/settings.png) |
| Network mode | WiFi selection | Settings |

---

## Original CrossPoint documentation

For hardware specs, standard EPUB reader features, development setup, contributing guidelines, and the full internals writeup, see **[CROSSPOINT-README.md](CROSSPOINT-README.md)**.

---

*CrossPoint OpenClaw is not affiliated with Xteink. Built on top of the open-source [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) project.*

# CrossPoint Reader — Laird's Fork

This is a fork of [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader), the open-source firmware for the **Xteink X4** e-paper reader (ESP32-C3).

**For standard features, installation, and the user guide, see the [upstream repository](https://github.com/crosspoint-reader/crosspoint-reader).**

## Philosophy

I use this fork to scratch my own itch — adding features I personally want on my reader, integrating it with my home AI setup ([OpenClaw](https://github.com/openclaw/openclaw)), and generally making it work the way I want it to work.

Because I believe in open source and want to give back to the project that made this possible, I commit to upstreaming anything that's generally useful. If I build something that could benefit other CrossPoint users, I open a PR. The features listed below are either already submitted upstream or on the list to be.

---

## Quick Start — Install Latest Release

**Latest Release**: v1.1.2-claw (2026-03-19)

[Download firmware.bin](https://github.com/laird/crosspoint-claw/releases/download/v1.1.2-claw/firmware.bin) | [Release Notes](https://github.com/laird/crosspoint-claw/releases/tag/v1.1.2-claw)

### Install in 2 Minutes (Web Installer)

1. **Enable Danger Zone** on your reader (Settings → System → Danger Zone → ON)
2. **Open** `http://<reader-ip>/` in your browser
3. **Upload** `firmware.bin` file
4. **Reboot** — device auto-flashes

📖 **[Full Installation Guide](#installation)** with alternatives (SD Card, USB)

---

## What's Different in This Fork

### PULSR Theme (PR [#1331](https://github.com/crosspoint-reader/crosspoint-reader/pull/1331))

A new visual design built for the Xteink X4's e-ink display.

- **4-segment left navigation bar** — compact icons replace the traditional bottom tab bar, freeing reading space
- **Antonio font** for headers — clean, legible sans-serif at all sizes  
- **PULSR Dark** — inverted variant for comfortable night/low-light reading
- Customisable tab labels with short abbreviations for compact mode

### OpenClaw Integration

The reader integrates with [OpenClaw](https://github.com/openclaw/openclaw), an AI personal assistant, enabling automated content delivery and device management.

#### Content Feed (`feat/rss-feed-sync`, PR [#1362](https://github.com/crosspoint-reader/crosspoint-reader/pull/1362)–[#1368](https://github.com/crosspoint-reader/crosspoint-reader/pull/1368))
- RSS/Atom feed server running on the NUC serves EPUBs, news briefings, and other content
- Reader polls the feed and syncs new files automatically over WiFi
- Daily erotic fiction, thought leadership articles, and road trip guides delivered overnight

#### Danger Zone (PR [#1368](https://github.com/crosspoint-reader/crosspoint-reader/pull/1368))
- Web-based device management interface accessible from the local network
- Screenshot tour: capture full-device screenshot sequences for debugging
- OTA firmware flashing via HTTP upload (no USB required)

#### Automatic Sync
- OpenClaw cron job polls both readers (192.168.0.234 and 192.168.0.194) every 15 minutes
- When readers come online, content and firmware sync automatically
- `crosspoint-feed/` staging directory mirrors to device on each sync

### On-Device Linking Support (PR [#1376](https://github.com/crosspoint-reader/crosspoint-reader/pull/1376)) — *draft*

Navigate links embedded in EPUB files using physical buttons — no touchscreen required.

- All `<a href>` links in an EPUB page are tracked with pixel-level bounding rects during layout
- **Down** button → enter link cursor mode (first link highlighted)
- **Up / Down** → cycle through links on the page with an inverted highlight
- **Confirm** → follow the focused link (`navigateToHref`)
- **Back** → exit cursor mode, return to reading
- "Footnotes" menu renamed to **"Links"** to reflect that it surfaces all inline links, not just footnotes
- Enables cross-referenced EPUB collections (e.g. SCP Foundation) to be navigated naturally

### OTA Improvements (PR [#1336](https://github.com/crosspoint-reader/crosspoint-reader/pull/1336))
- GitHub release assets redirect to CDN — follow up to 5 redirects (`max_redirection_count=5`)
- `file.flush()` before close ensures firmware file is visible after download completes
- Progress callback wired to render live progress bar during download
- Boot partition marked valid early to prevent rollback on reboot

### Nav Arrows (PR [#1362](https://github.com/crosspoint-reader/crosspoint-reader/pull/1362))
- Left/right arrows display correctly in reader and settings tab navigation

### Web UI Improvements (PR [#1364](https://github.com/crosspoint-reader/crosspoint-reader/pull/1364))
- File transfer UI shows upload progress status
- Long filenames truncated with ellipsis in the received file list

### Status Bar Clock (`feat/status-bar-clock`)
- Optional clock display in the reader status bar

### File Browser Sort (`feat/file-browser-sort`)
- Sort files alphabetically or by date in the file picker

---

## Content Automation

This fork is paired with OpenClaw automations that run on a home server:

| Cron | Schedule | What it does |
|------|----------|--------------|
| `daily-erotic-story` | 4:00 AM | Generates a new erotic fiction story (GLM-5), saves as EPUB |
| `daily-erotic-art` | 9:00 AM | Fetches public-domain art from Wikimedia, converts to sleep screen BMP |
| `morning-audio-briefing` | 6:20 AM | Generates daily news briefing podcast (ChipCast) |
| `daily-trip-suggestion` | 8:00 AM Wed | Road trip suggestion with illustrated poster |
| `maxs-book-update` | 6:00 AM Wed | Checks SpaceBattles for new chapters of Max's book, rebuilds EPUB |
| `reader-sync` | Every 15 min | Polls readers; syncs new content and firmware when online |

### Content Library

The following libraries are staged and delivered automatically:

- **SCP Foundation** — 153 EPUBs from [lselden/scp-to-epub](https://github.com/lselden/scp-to-epub), organised into Series, Canons, Groups, International, Wanderers, BestOf
- **AO3 erotic fiction** — organised by genre (Alien-SciFi, BDSM-Kink, Paranormal, LGBTQ, Romance)
- **AI-generated stories** — saved to `Books/chip/YYYY-MM/` monthly subdirs
- **Thought leadership articles** — EPUB versions of published articles
- **WhatsNew.epub** — navigable index of recently added content, updated automatically

---

## Installation

### Option 1: Web Installer (Easiest - No USB Required)

This method uses the reader's built-in web interface to install firmware over WiFi.

#### Prerequisites
- Reader connected to WiFi
- Firmware file (`firmware.bin`) accessible on your local network
- Reader's IP address (e.g., 192.168.0.234)

#### Steps

**1. Enable Danger Zone (One-time setup)**

On your reader:
- Go to **Settings → System → Danger Zone**
- Toggle **ON** to enable web-based device management
- Note the password (default: `1814`)

**2. Upload Firmware via Web UI**

Using any browser on your network:

```
http://<reader-ip>/
```

Example: `http://192.168.0.234/`

Navigate to:
- **Settings** → **Firmware Upload** 
- Select your `firmware.bin` file
- Click **Upload**
- Device will show upload progress
- When complete, device will automatically reboot and flash

**3. Verify Installation**

After reboot (1-2 minutes):
```bash
curl http://192.168.0.234/api/status | jq '.version'
```

Expected output: `"1.1.2-claw-dev+feature/cover-browser"` (or current version)

---

#### Automated Upload Script

For scripted/automated deployments:

```bash
#!/bin/bash
READER_IP="192.168.0.234"
DZ_PASSWORD="1814"
FIRMWARE="firmware.bin"

# Enable Danger Zone (if not already enabled)
# curl -X POST "http://${READER_IP}/api/danger-zone/enable?password=${DZ_PASSWORD}"

# Upload firmware
echo "Uploading firmware..."
curl -X POST "http://${READER_IP}/upload?path=/" \
  -F "file=@${FIRMWARE};filename=firmware.bin"

# Verify upload completed
sleep 2
echo "Verifying upload..."
curl -s "http://${READER_IP}/api/files?path=/" | \
  jq -r '.[] | select(.name=="firmware.bin") | .size'

# Reboot to trigger flash
echo "Rebooting reader..."
curl -X POST "http://${READER_IP}/api/reboot?password=${DZ_PASSWORD}"

echo "Firmware flashing in progress. Device will reboot shortly..."
sleep 30

# Verify new version
echo "Verifying installation..."
curl -s "http://${READER_IP}/api/status" | jq '.version'
```

---

### Option 2: SD Card (Most Reliable)

If the web interface is unavailable or you prefer a failsafe method.

#### Steps

1. **Copy firmware to SD card**
   ```bash
   cp firmware.bin /path/to/sd-card/
   ```
   
2. **Insert SD card into reader**
   - Power off the reader
   - Insert SD card with `firmware.bin` at root
   - Power on

3. **Device auto-flashes**
   - Bootloader detects `firmware.bin` on startup
   - Flashing progress displays on screen (~30 seconds)
   - Device reboots with new firmware
   - `firmware.bin` is consumed/deleted after successful flash

---

### Option 3: USB (Developer/Fallback)

For development or if WiFi/SD methods fail.

#### Prerequisites
- USB cable (micro-USB to your computer)
- `esptool.py` installed: `pip install esptool`
- `firmware.bin` on your computer

#### Steps

```bash
esptool.py --chip esp32c3 \
  --port /dev/ttyACM0 \
  write_flash -z -fm dio -ff 40m \
  0x0 firmware.bin
```

**macOS:**
```bash
esptool.py --chip esp32c3 \
  --port /dev/cu.usbmodem* \
  write_flash -z -fm dio -ff 40m \
  0x0 firmware.bin
```

**Windows (PowerShell):**
```powershell
esptool.py --chip esp32c3 --port COM3 write_flash -z -fm dio -ff 40m 0x0 firmware.bin
```

---

## Building

See the [upstream development guide](https://github.com/crosspoint-reader/crosspoint-reader#development).

```bash
git clone https://github.com/laird/crosspoint-claw
cd crosspoint-claw

# Clone the SDK as a sibling directory (required — platformio.ini
# references libraries via symlink://open-x4-sdk/...)
git clone https://github.com/nicx-x4/open-x4-sdk ../open-x4-sdk

pio run
```

The `feature/claw` branch is the main integration branch. All upstream PRs are also available as standalone branches.

---

## Open PRs Upstream

| PR | Status | Description |
|----|--------|-------------|
| [#1331](https://github.com/crosspoint-reader/crosspoint-reader/pull/1331) | Open | PULSR theme — 4-segment left nav bar with Antonio font |
| [#1336](https://github.com/crosspoint-reader/crosspoint-reader/pull/1336) | Open | OTA improvements — redirect follow, progress bar, flush fix |
| [#1362](https://github.com/crosspoint-reader/crosspoint-reader/pull/1362) | Open | Nav arrows — correct left/right display in reader/settings |
| [#1364](https://github.com/crosspoint-reader/crosspoint-reader/pull/1364) | Open | Web UI improvements — upload status, filename truncation |
| [#1368](https://github.com/crosspoint-reader/crosspoint-reader/pull/1368) | Open | Danger Zone — web-based device management |
| [#1376](https://github.com/crosspoint-reader/crosspoint-reader/pull/1376) | Draft | On-device linking support for EPUB |

---

*This fork is maintained by [@laird](https://github.com/laird). Upstream project: [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader).*

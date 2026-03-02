# CrossPoint Development Workflow

A guide to the full development lifecycle for CrossPoint firmware — from getting started to deploying updates, debugging in the field, and using RSS-based OTA to streamline iterations.

---

## Getting Started

### The Fast Path: Ask an AI to Set It Up

You can hand this document to Claude Code or OpenClaw and ask it to get you up and running. Paste this prompt:

```
I want to contribute to the CrossPoint Reader firmware project.
Read docs/development-workflow.md for context, then:

1. Install PlatformIO CLI if not already present (pip install platformio)
2. Clone the repo: https://github.com/YOUR_USERNAME/crosspoint-reader
   (or use the fork: https://github.com/YOUR_USERNAME/crosspoint-reader)
3. Initialize git submodules (open-x4-sdk is required)
4. Run a full build with `pio run` and confirm it succeeds
5. Report the firmware size (RAM % and Flash % used)
6. Tell me what the SD card OTA workflow is and what to do next

If anything fails, diagnose and fix it before reporting back.
```

Claude Code will handle the install, clone, submodule init, and build end-to-end. If it hits missing dependencies or Python version mismatches it'll fix them. You'll get back a confirmed build and clear next steps for flashing.

---

### Manual Setup

### Prerequisites

- **PlatformIO** (VS Code extension or CLI)
- **Python 3** (for i18n code generation)
- **USB cable** (for initial flash and serial monitoring)

### Clone and Build

```bash
git clone https://github.com/YOUR_USERNAME/crosspoint-reader.git
cd crosspoint-reader
git submodule update --init --recursive

# Build firmware
pio run

# Flash via USB (first time, or when SD card OTA isn't set up yet)
pio run --target upload
```

### Safe USB Flash (Recommended for Development)

For development, use `scripts/crosspoint-flash.py` — a safe OTA-aware flash script that always writes to the **inactive** partition and preserves the running firmware as a fallback:

```bash
# Prerequisites: esptool installed in a venv on the machine with USB access
pip install esptool   # or use an existing venv

# Check which partition is active (no flash)
python3 scripts/crosspoint-flash.py --status --port /dev/cu.usbmodem101

# Build and flash safely (from repo root, on Linux/Mac with NUC + iMac setup)
scripts/flash-crosspoint.sh

# Or flash a pre-built binary directly
python3 scripts/crosspoint-flash.py .pio/build/default/firmware.bin --port /dev/cu.usbmodem101
```

**What it does:**
1. Reads `otadata` to determine which partition (app0/app1) is currently active
2. Flashes new firmware to the **inactive** partition only
3. Updates `otadata` to boot from the new partition
4. Resets the device

**Safety:** If the new firmware crashes before calling `esp_ota_mark_app_valid_cancel_rollback()`, the ESP-IDF bootloader automatically rolls back to the previous partition. The old firmware is never overwritten.

> ⚠️ **Always commit before flashing.** The feed server serves firmware identified by SHA. If you flash uncommitted code (same SHA as parent commit), the feed will re-download and re-flash the old firmware on next sync, undoing your work. `flash-crosspoint.sh` enforces this with a pre-flight check.

### SD Card OTA (Preferred After First Flash)

Once the device has CrossPoint firmware, you never need USB again. Copy firmware to the SD card instead:

```bash
# Build and copy to SD card
pio run

# Find your SD card mount point (it changes every reconnect!)
lsblk -rno NAME,MOUNTPOINT | grep -v "^$"

# Copy firmware (replace /media/YOUR_USER/SDCARD with your actual mount point)
SDCARD=/media/YOUR_USER/SDCARD
cp .pio/build/default/firmware.bin "$SDCARD/firmware.bin"
ls -lh "$SDCARD/firmware.bin"   # verify it landed
udisksctl unmount -b /dev/sdX   # eject safely
```

On next boot, the device detects `/firmware.bin`, flashes to the **inactive** partition, updates `otadata`, deletes the file, and reboots. If the new firmware fails, the bootloader rolls back automatically.

---

## Serial Debugging

Connect via USB while the device is running to see live logs:

```bash
# Linux/Mac
python3 scripts/debugging_monitor.py

# macOS (adjust port)
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

The RSS sync module logs all activity to Serial in real time:
```
[RssFeedSync] Starting sync from https://example.com/feed.xml
[RssFeedSync] Fetched feed (HTTP 200, 4321 bytes)
[RssFeedSync] Item: guid=abc123 type=file path=/Books/fiction/ → downloading...
[RssFeedSync] ✓ Downloaded: story-title.epub (45.2KB)
[RssFeedSync] Item: guid=def456 type=news → updating News.md
[RssFeedSync] Sync complete: 3 downloaded, 2 skipped, 0 errors
```

---

## RSS Feed Sync

### Overview

When the device connects to a non-hotspot WiFi network, it automatically checks a configured RSS feed URL for new content and downloads it in the background — without blocking the web UI or reader.

### What the Feed Can Deliver

| Type | Description | Destination |
|------|-------------|-------------|
| `file` | EPUB, Markdown, or any document | Specified path (e.g. `/Books/fiction/`) |
| `image` | BMP sleep screen art | `/sleep/` |
| `news` | News stories | Aggregated into `/News.md` (newest first) |
| `firmware` | Firmware binary | `/firmware.bin` → OTA on next boot |

### Configuring the Feed URL

The feed URL is set **only via the web interface** — not the on-device menu. This is intentional: it's a sensitive setting that should be configured deliberately.

1. Connect the device to WiFi (or use Hotspot mode)
2. Open the web UI in a browser (URL shown on the device screen)
3. Navigate to **Settings → Feed Sync**
4. Enter your feed URL and save

If no feed URL is set, the device shows: *"Set feed URL in web interface to enable auto-sync"*

### SD Card Log File

Every sync run appends to `/.crosspoint/feed-sync.log` on the SD card. This is your primary debugging tool for field issues:

```
2026-02-27T12:00:01Z [INFO] Sync started. Feed: https://example.com/...xml
2026-02-27T12:00:02Z [INFO] HTTP 200, 3421 bytes received
2026-02-27T12:00:02Z [INFO] guid=abc123 type=file → /Books/fiction/story.epub → OK (45.2KB)
2026-02-27T12:00:03Z [INFO] guid=def456 type=news → News.md → OK
2026-02-27T12:00:03Z [SKIP] guid=ghi789 already seen
2026-02-27T12:00:03Z [INFO] Sync complete: 2 downloaded, 1 skipped, 0 errors
```

Pull the SD card and open this file when you need to diagnose why something didn't sync. The log is capped at 50KB (oldest entries trimmed automatically).

---

## OTA Firmware Updates via RSS

### ⚠️ Security Warning

**Only enable firmware OTA if YOU control the feed.** A firmware update installs arbitrary code on the device with no verification. If someone else controls the feed URL, they control your device. This is not a setting to enable casually.

The `FEED_ALLOW_FIRMWARE` setting defaults to **off**. Enable it only if:
- You are writing the firmware yourself
- You host the feed URL on infrastructure you control
- You understand that enabling this gives the feed owner full control of the device

### Setting Up a Simple OTA Feed

The simplest possible OTA setup is a static file on a web server. Python's built-in HTTP server works fine for local development:

**Step 1: Build firmware**
```bash
cd crosspoint-reader
pio run
cp .pio/build/default/firmware.bin /var/www/crosspoint/firmware.bin
```

**Step 2: Create a feed**

Save as `/var/www/crosspoint/feed.xml`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<rss version="2.0" xmlns:crosspoint="https://crosspoint.example/ns">
  <channel>
    <title>CrossPoint Updates</title>
    <link>https://your-server.example/crosspoint/</link>
    <description>Firmware and content updates</description>

    <item>
      <title>CrossPoint v1.2.0 - RSS Sync Feature</title>
      <crosspoint:type>firmware</crosspoint:type>
      <enclosure
        url="https://your-server.example/crosspoint/firmware.bin"
        type="application/octet-stream"
        length="750000"/>
      <guid>firmware-v1.2.0</guid>
      <pubDate>Fri, 27 Feb 2026 12:00:00 EST</pubDate>
    </item>

  </channel>
</rss>
```

**Step 3: Serve it**

Any static file server works. Python's built-in:
```bash
cd /var/www/crosspoint
python3 -m http.server 8080
```

Or Apache/Nginx/Caddy for a production setup.

**Step 4: Configure the device**

1. Open web UI → Settings → Feed Sync
2. Set Feed URL: `http://your-server:8080/feed.xml`
3. Enable "Allow firmware updates" (**read the warning**)
4. Save

**Step 5: Deploy**

Next time the device connects to WiFi, it:
1. Fetches `feed.xml`
2. Sees the firmware item (new guid)
3. Downloads `firmware.bin` to SD card root
4. On next boot: flashes itself automatically

No USB needed. Update your `feed.xml` with a new `<guid>` each release to trigger a re-download.

### Guid Strategy for Firmware Releases

The device deduplicates by guid — it won't re-download if it's already seen the guid. Use version numbers:

```xml
<guid>firmware-v1.2.0</guid>   <!-- triggers once -->
<guid>firmware-v1.2.1</guid>   <!-- triggers again on next release -->
```

---

## Iterative Development Loop

With all of this in place, the development cycle is:

```
Write code → pio run → copy firmware.bin to feed server 
→ update feed.xml with new guid → device syncs automatically on WiFi
→ check feed-sync.log for delivery confirmation
→ monitor Serial for runtime logs
→ repeat
```

No USB flashing after the first setup. No physical access to the device needed. Full audit trail in the log file.

---

## Screen Captures

The CrossPoint web interface includes a screen capture endpoint for debugging UI layouts:

```bash
# Capture current screen as PNG
curl http://crosspoint.local/api/screenshot > screen.png

# Or with IP address
curl http://192.168.x.x/api/screenshot > screen.png
```

Use this during development to:
- Verify UI changes look right without picking up the device
- Attach screenshots to bug reports
- Document UI states for design review

---

## Troubleshooting Checklist

| Symptom | Where to look |
|---------|---------------|
| Sync not starting | Check feed URL is set in web UI |
| Feed fetched but no files | Check `feed-sync.log` for HTTP errors |
| File not appearing on device | Check `feed-sync.log` for path/write errors |
| Firmware not installing | Confirm `FEED_ALLOW_FIRMWARE` is enabled; check `/firmware.bin` exists on SD |
| News.md not updating | Check `feed-sync.log` for parse/write errors |
| Device not syncing at all | Verify WiFi is STA mode (not hotspot); check Serial logs on connect |

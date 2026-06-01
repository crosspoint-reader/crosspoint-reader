# CrossPoint User Guide

Welcome to the **CrossPoint** firmware. This guide outlines the hardware controls, navigation, and reading features of the device.

- [CrossPoint User Guide](#crosspoint-user-guide)
  - [1. Hardware Overview](#1-hardware-overview)
    - [Button Layout](#button-layout)
  - [2. Power \& Startup](#2-power--startup)
    - [Power On / Off](#power-on--off)
    - [First Launch](#first-launch)
  - [3. Screens](#3-screens)
    - [3.1 Home Screen](#31-home-screen)
    - [3.2 Reading Mode](#32-reading-mode)
    - [3.3 Browse Files Screen](#33-browse-files-screen)
    - [3.4 Recent Books Screen](#34-recent-books-screen)
    - [3.5 File Transfer Screen](#35-file-transfer-screen)
      - [3.5.1 Calibre Wireless Transfers](#351-calibre-wireless-transfers)
    - [3.6 Settings](#36-settings)
      - [3.6.1 Display](#361-display)
      - [3.6.2 Reader](#362-reader)
      - [3.6.3 Controls](#363-controls)
      - [3.6.4 System](#364-system)
      - [3.6.5 OPDS Servers (Multiple Libraries)](#365-opds-servers-multiple-libraries)
      - [3.6.6 Web Settings (WiFi + OPDS)](#366-web-settings-wifi--opds)
      - [3.6.7 KOReader Sync Quick Setup](#367-koreader-sync-quick-setup)
    - [3.7 Sleep Screen](#37-sleep-screen)
    - [3.8 Custom Fonts (SD Card)](#38-custom-fonts-sd-card)
  - [4. Reading Mode](#4-reading-mode)
    - [Page Turning](#page-turning)
    - [Chapter Navigation](#chapter-navigation)
    - [Auto Page Turn](#auto-page-turn)
    - [Tilt Page Turn (X3 only)](#tilt-page-turn-x3-only)
    - [Footnote Navigation](#footnote-navigation)
    - [System Navigation](#system-navigation)
    - [Supported Languages](#supported-languages)
  - [5. Reader Menu](`#5-reader-menu`)
    - [5.1 Chapter Selection](`#51-chapter-selection`)
    - [5.2 Bookmarks](`#52-bookmarks`)
  - [6. Current Limitations & Roadmap](`#6-current-limitations--roadmap`)
  - [7. Troubleshooting Issues & Escaping Bootloop](`#7-troubleshooting-issues--escaping-bootloop`)

## 1. Hardware Overview

The device utilises the standard buttons on the Xteink X4 (in the same layout as the manufacturer firmware, by default):

### Button Layout

| Location        | Buttons                                              |
| --------------- | ---------------------------------------------------- |
| **Bottom Edge** | **Back**, **Confirm**, **Left**, **Right**           |
| **Right Side**  | **Power**, **Volume Up**, **Volume Down**, **Reset** |

Button layout can be customized in the **[Controls Settings](#363-controls)**.

### Taking a Screenshot

When the Power Button and Volume Down button are pressed at the same time, it will take a screenshot and save it in the folder `screenshots/`.

Alternatively, while reading a book, press the **Confirm** button to open the reader menu and select **Take screenshot**.

---

## 2. Power & Startup

### Power On / Off

To turn the device on or off, **press and hold the Power button for approximately half a second**.
In the **[Controls Settings](#363-controls)** you can configure the power button to turn the device off with a short press instead of a long one.

To reboot the device (for example after a firmware update or if it's frozen), press and release the Reset button, and then quickly press and hold the Power button for a few seconds.

### First Launch

Upon turning the device on for the first time, you will be placed on the **[Home](#31-home-screen)** screen.

> [!NOTE]
> On subsequent restarts, the firmware will automatically reopen the last book you were reading.

---

## 3. Screens

### 3.1 Home Screen

The Home screen is the main entry point to the firmware. From here you can navigate to **[Reading Mode](#4-reading-mode)** with the most recently read book, the **[Browse Files](#33-browse-files-screen)** screen, the **[Recent Books](#34-recent-books-screen)** screen, the **[File Transfer](#35-file-transfer-screen)** screen, or **[Settings](#36-settings)**.

### 3.2 Reading Mode

See [Reading Mode](#4-reading-mode) below for more information.

### 3.3 Browse Files Screen

The Browse Files screen acts as a file and folder browser. The full path to the current directory is shown at the top of the screen. File extensions are displayed alongside each filename, and directories are shown with brackets (e.g. `[folder-name]`). Hidden directories (those beginning with `.`) are also visible.

* **Navigate List:** Use **Left** (or **Volume Up**), or **Right** (or **Volume Down**) to move the selection cursor up and down through folders and books. You can also long-press these buttons to scroll a full page up or down.
* **Open Selection:** Press **Confirm** to open a folder or start reading a selected book. Selecting a `.bmp` file will open the image viewer.
* **Delete Files or Folders:** Hold and release **Confirm** to delete the selected file or folder. You will be given an option to either confirm or cancel. Multiple files can be selected for deletion in a single operation.
* **Rename or Move:** Files can be renamed or moved to a different folder from within the browse screen.

### 3.4 Recent Books Screen

The Recent Books screen lists the most recently opened books in a chronological view, displaying title and author.

### 3.5 File Transfer Screen

The File Transfer screen allows you to upload new e-books to the device. When you enter the screen, you'll be prompted with a WiFi selection dialog and then your X4 will start hosting a web server.

See the [webserver docs](./docs/webserver.md) for more information on how to connect to the web server and upload files.

The web interface also supports **WebDAV**, allowing you to mount the device as a network drive and manage files directly from your computer's file manager.

Download links for files already on the device are available in the web interface, so you can retrieve books or screenshots over WiFi without connecting a cable.

A **WiFi signal strength indicator** (dBm) is displayed on-screen during web server sessions.

> [!TIP]
> Advanced users can also manage files programmatically or via the command line using `curl`. See the [webserver docs](./docs/webserver.md) for details.
> [!TIP]
> If your EPUBs have compatibility issues, you can run the built-in **EPUB Optimizer** directly from the device to clean up and reprocess books for better rendering.

### 3.5.1 Calibre Wireless Transfers

CrossPoint supports sending books from Calibre using the CrossPoint Reader device plugin.

1. Install the plugin in Calibre:
   
   - Head to https://github.com/crosspoint-reader/calibre-plugins/releases to download the latest version of the crosspoint_reader plugin.
   
   - Download the zip file.
   
   - Open Calibre → Preferences → Plugins → Load plugin from file → Select the zip file.

2. On the device: File Transfer → Connect to Calibre → Join a network.

3. Make sure your computer is on the same WiFi network.

4. In Calibre, click "Send to device" to transfer books.

### 3.6 Settings

The Settings screen allows you to configure the device's behavior. There are a few settings you can adjust:

#### 3.6.1 Display

- **Sleep Screen**: Which sleep screen to display when the device sleeps:
  
  - "Dark" (default) - The default dark Crosspoint logo sleep screen
  - "Light" - The same default sleep screen, on a white background
  - "Custom" - Custom images from the SD card; see [Sleep Screen](#37-sleep-screen) below for more information
  - "Cover" - The book cover image (Note: this is experimental and may not work as expected)
  - "None" - A blank screen
  - "Cover + Custom" - The book cover image while actively reading, falls back to "Custom" behavior otherwise

- **Sleep Screen Cover Mode**: How to display the book cover when "Cover" sleep screen is selected:
  
  - "Fit" (default) - Scale the image down to fit centered on the screen, padding with white borders as necessary
  - "Crop" - Scale the image down and crop as necessary to try to fill the screen (Note: this is experimental and may not work as expected)

- **Sleep Screen Cover Filter**: What filter will be applied to the book cover when "Cover" sleep screen is selected:
  
  - "None" (default) - The cover image will be converted to a grayscale image and displayed as it is
  - "Contrast" - The image will be displayed as a black & white image without grayscale conversion
  - "Inverted" - The image will be inverted as in white & black and will be displayed without grayscale conversion

- **Status Bar**: Configure the status bar displayed while reading:
  
  - "None" - No status bar
  - "No Progress" - Show status bar without reading progress
  - "Full w/ Percentage" - Show status bar with book progress (as percentage)
  - "Full w/ Book Bar" - Show status bar with book progress (as bar)
  - "Book Bar Only" - Show book progress (as bar)
  - "Full w/ Chapter Bar" - Show status bar with chapter progress (as bar)

- **Hide Battery %**: Configure where to suppress the battery percentage display in the status bar; the battery icon will still be shown:
  
  - "Never" (default) - Always show battery percentage
  - "In Reader" - Show battery percentage everywhere except in reading mode
  - "Always" - Always hide battery percentage

- **Refresh Frequency**: Set how often the screen does a full refresh while reading to reduce ghosting; options are every 1, 5, 10, 15, or 30 pages.

- **UI Theme**: Set which UI theme to use:
  
  - "Classic" - The original Crosspoint theme
  - "Lyra" - The new theme for Crosspoint featuring rounded elements and menu icons
  - "Lyra Extended" - Lyra, but displays 3 books instead of 1 on the **[Home Screen](#31-home-screen)**
  - "RoundedRaff" - A rounded theme with additional visual styling

- **Sunlight Fading Fix**: Configure whether to enable a software-fix for the issue where white X4 models may fade when used in direct sunlight:
  
  - "OFF" (default) - Disable the fix
  - "ON" - Enable the fix

> [!NOTE]
> A battery charging indicator is shown on the battery icon whenever the device is actively charging.

#### 3.6.2 Reader

- **Reader Font Family**: Choose the font used for reading:
  
  - "Noto Serif" (default) - Google's serif font
  - "Noto Sans" - Google's sans-serif font
  - "Open Dyslexic" - Font designed for readers with dyslexia

- **Reader Font Size**: Adjust the text size for reading; options are "Small", "Medium" (default), "Large", or "X Large".

- **Reader Line Spacing**: Adjust the spacing between lines; options are "Tight", "Normal" (default), or "Wide".

- **Reader Screen Margin**: Controls the screen margins in Reading Mode between 5 and 40 pixels in 5-pixel increments.

- **Reader Paragraph Alignment**: Set the alignment of paragraphs; options are "Justified" (default), "Left", "Center", or "Right".

- **Embedded Style**: Whether to use the EPUB file's embedded HTML and CSS stylisation and formatting; options are "ON" or "OFF".

- **Hyphenation**: Whether to hyphenate text in Reading Mode; options are "ON" or "OFF".

- **Reading Orientation**: Set the screen orientation for reading EPUB files:
  
  - "Portrait" (default) - Standard portrait orientation
  - "Landscape CW" - Landscape, rotated clockwise
  - "Inverted" - Portrait, upside down
  - "Landscape CCW" - Landscape, rotated counter-clockwise

- **Extra Paragraph Spacing**: Set how to handle paragraph breaks:
  
  - "ON" - Vertical space will be added between paragraphs in Reading Mode
  - "OFF" - Paragraphs will not have vertical space added, but will have first-line indentation

- **Text Anti-Aliasing**: Whether to show smooth grey edges (anti-aliasing) on text in reading mode. Note this slows down page turns slightly.

- **Images**: Whether to display embedded images (JPG/PNG) found in EPUB files; options are "ON" (default) or "OFF".

- **Focus Reading**: Bolds the first part of each word to create visual fixation points, similar to Bionic Reading. This can help improve reading speed and focus; options are "ON" or "OFF" (default).

#### 3.6.3 Controls

- **Remap Front Buttons**: A menu for customising the function of each bottom edge button.

- **Side Button Layout (reader)**: Swap the order of the up and down volume buttons from "Prev/Next" (default) to "Next/Prev". You can also disable them entirely. This change is only in effect when reading.

- **Long-press Chapter Skip**: Set whether long-pressing page turn buttons skips to the next/previous chapter:
  
  - "Chapter Skip" (default) - Long-pressing skips to next/previous chapter
  - "Page Scroll" - Long-pressing scrolls a page up/down

- **Short Power Button Click**: Controls the effect of a short click of the power button:
  
  - "Ignore" (default) - Require a long press to turn off the device
  - "Sleep" - A short press puts the device into sleep mode
  - "Page Turn" - A short press in reading mode turns to the next page; a long press turns the device off
  - "Refresh" - A short press triggers a manual full-screen refresh, useful for clearing ghosting

#### 3.6.4 System

- **Time to Sleep**: Set the duration of inactivity before the device automatically goes to sleep; options are 1, 3, 5, 10 (default), 15 or 30 minutes.

- **WiFi Networks**: Connect to WiFi networks for file transfers and firmware updates.

- **KOReader Sync**: Options for setting up KOReader for syncing book progress.

- **OPDS Servers**: Manage one or more OPDS [(Open Publication Distribution System)](https://en.wikipedia.org/wiki/Open_Publication_Distribution_System) libraries for browsing and downloading books. See [OPDS Servers (Multiple Libraries)](#365-opds-servers-multiple-libraries) below.

- **Clear Reading Cache**: Clear the internal SD card cache.

- **Check for updates**: Check for Crosspoint firmware updates over WiFi. Firmware can also be updated without a USB connection by placing a `firmware.bin` file on the SD card.

- **Language**: Set the UI language. CrossPoint supports 22 languages including English, Spanish, French, German, Czech, Portuguese, Russian, Swedish, Turkish, Danish, Finnish, Polish, Dutch, Belarusian, Italian, Ukrainian, Romanian, Catalan, Vietnamese, Kazakh, Slovenian, and more.

- **Manage Fonts**: Browse, download, and manage custom font families installed from the SD card. See [Custom Fonts (SD Card)](#38-custom-fonts-sd-card) for more information.

#### 3.6.5 OPDS Servers (Multiple Libraries)

CrossPoint supports saving multiple OPDS servers and switching between them when browsing catalogs.

1. Open **Settings -> System -> OPDS Servers**.

2. Select **Add Server** to create a new entry, or select an existing server to edit it.

3. Configure these fields:
   
   - **Server Name**: Optional display name (for example, "Home Calibre" or "Public Catalog").
   
   - **OPDS Server URL**: Full catalog root URL (for Calibre Content Server, usually ends with `/opds`).
   
   - **Username / Password**: Optional credentials for authenticated servers.

4. Use **Delete Server** inside a server entry to remove it.

Behavior notes:

- You can store up to 8 OPDS servers.
- OPDS authentication supports HTTP Basic auth. If you use Calibre Content Server with authentication enabled, set it to Basic (not Digest).

You can also manage OPDS servers from the web interface while in File Transfer mode:

1. Connect to the device web UI.
2. Open `http://<device-ip>/settings`.
3. Use the **OPDS Servers** card to add, edit, or delete entries.

For web-based WiFi network management, see [Web Settings (WiFi + OPDS)](#366-web-settings-wifi--opds).

#### 3.6.6 Web Settings (WiFi + OPDS)

While in **File Transfer** mode, the web settings page includes management cards for both **WiFi Networks** and **OPDS Servers**.

1. On device: open **File Transfer** and connect to WiFi.
2. In a browser, open `http://<device-ip>/settings` or `http://crosspoint.local`.
3. In **WiFi Networks**, add, edit, or delete saved network entries (SSID + optional password).
4. In **OPDS Servers**, add, edit, or delete OPDS catalogs.

Behavior notes:

- Passwords are never shown back in the web UI after saving.
- Leaving Password blank while editing keeps the existing saved password unchanged.
- The web UI can save hidden-network SSIDs, but connecting to hidden networks still depends on device-side WiFi connection flow.

#### 3.6.7 KOReader Sync Quick Setup

CrossPoint can sync reading progress with KOReader-compatible sync servers.
It also interoperates with KOReader apps/devices when they use the same server and credentials.

##### Option A: Free Public Server (`sync.koreader.rocks`)

1. Register a user once (only if needed):

```bash
USERNAME="user"
PASSWORD="pass"
PASSWORD_MD5="$(printf '%s' "$PASSWORD" | openssl md5 | awk '{print $2}')"

curl -i "https://sync.koreader.rocks/users/create" \
  -H "Accept: application/vnd.koreader.v1+json" \
  -H "Content-Type: application/json" \
  --data "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD_MD5\"}"
```

Already have KOReader Sync credentials? Skip registration; basic sync only requires using the same existing username/password on all devices.

When this returns `HTTP 402` with `{"code":2002,"message":"Username is already registered."}`, pick a different username or use that existing account.

2. On each CrossPoint device:
   
   - Go to **Settings -> System -> KOReader Sync**.
   
   - Set **Username** and **Password** (enter the plain password; CrossPoint computes MD5 internally, and use the same values on all devices).
   
   - Set **Sync Server URL** to `https://sync.koreader.rocks`, or leave it empty (both use the same default KOReader sync server).
   
   - Run **Authenticate**.

3. While reading, press **Confirm** to open the reader menu, then select **Sync Progress**.
   
   - Choose **Apply Remote** to jump to remote progress.
   
   - Choose **Upload Local** to push current progress.

##### Option B: Self-Hosted Server (Docker Compose)

1. Start a sync server:

```bash
mkdir -p kosync-quickstart
cd kosync-quickstart

cat > compose.yaml <<'YAML'
services:
  kosync:
    image: koreader/kosync:latest
    ports:
      - "7200:7200"
      - "17200:17200"
    volumes:
      - ./data/redis:/var/lib/redis
    environment:
      - ENABLE_USER_REGISTRATION=true
    restart: unless-stopped
YAML

# Docker
docker compose up -d

# Podman (alternative)
podman compose up -d
```

> [!NOTE]
> `ENABLE_USER_REGISTRATION=true` is convenient for first setup. After creating your users, set it to `false` (or remove it) to avoid unexpected registrations.

2. Verify the server:

```bash
curl -H "Accept: application/vnd.koreader.v1+json" "http://<server-ip>:17200/healthcheck"
# Expected: {"state":"OK"}
```

3. Register a user once.
   CrossPoint authenticates against KOReader Sync (`koreader/kosync`) using an MD5 key, so register using the MD5 of your password:

> [!WARNING]
> Sending a reusable MD5-derived password over plain HTTP is insecure.
> Create unique sync-only credentials and do not reuse main account passwords.
> Prefer `https://<server-ip>:7200` whenever traffic leaves a fully trusted LAN or when using untrusted networks.
> Use `curl -k` only for self-signed certificate testing.

```bash
USERNAME="user"
PASSWORD="pass"
PASSWORD_MD5="$(printf '%s' "$PASSWORD" | openssl md5 | awk '{print $2}')"

curl -i "http://<server-ip>:17200/users/create" \
  -H "Accept: application/vnd.koreader.v1+json" \
  -H "Content-Type: application/json" \
  --data "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD_MD5\"}"
```

If this returns `HTTP 402` with `{"code":2002,"message":"Username is already registered."}`, the account already exists.

4. On each CrossPoint device:
   
   - Go to **Settings -> System -> KOReader Sync**.
   
   - Set **Username** and **Password** (enter the plain password; CrossPoint computes MD5 internally, and use the same values on all devices).
   
   - Set **Sync Server URL** to `http://<server-ip>:17200`.
   
   - Run **Authenticate**.

If you use the HTTPS listener, use `https://<server-ip>:7200` (`curl -k` only for self-signed certificate testing).

5. While reading, press **Confirm** to open the reader menu, then select **Sync Progress**.
   
   - Choose **Apply Remote** to jump to remote progress.
   
   - Choose **Upload Local** to push current progress.

### 3.7 Sleep Screen

The **Sleep Screen** setting controls what is displayed when the device goes to sleep:

| Mode               | Behavior                                                                                                                     |
| ------------------ | ---------------------------------------------------------------------------------------------------------------------------- |
| **Dark** (default) | The CrossPoint logo on a dark background.                                                                                    |
| **Light**          | The CrossPoint logo on a white background.                                                                                   |
| **Custom**         | A custom image from the SD card (see below). Falls back to **Dark** if no custom image is found.                             |
| **Cover**          | The cover of the currently open book. Falls back to **Dark** if no book is open.                                             |
| **Cover + Custom** | The cover of the currently open book, shown only while actively reading. Falls back to **Custom** behavior when not reading. |
| **None**           | A blank screen.                                                                                                              |

#### Cover settings

When using **Cover** or **Cover + Custom**, two additional settings apply:

- **Sleep Screen Cover Mode**: **Fit** (scale to fit, white borders) or **Crop** (scale and crop to fill the screen).
- **Sleep Screen Cover Filter**: **None** (grayscale), **Contrast** (black & white), or **Inverted** (inverted black & white).

#### Custom images

To use custom sleep images, set the sleep screen mode to **Custom** or **Cover + Custom**, then place images on the SD card:

- **Multiple Images (recommended):** Create a `.sleep` directory in the root of the SD card and place any number of `.bmp` images inside. One will be randomly selected each time the device sleeps. (A directory named `sleep` is also accepted as a fallback.)
- **Single Image:** Place a file named `sleep.bmp` in the root directory. This is used as a fallback if no valid images are found in the `.sleep`/`sleep` directory.

> [!TIP]
> For best results:
> 
> - Use uncompressed BMP files with 24-bit color depth
> - X4: Use a resolution of 480x800 pixels to match the device's screen resolution.
> - X3: Use a resolution of 528x792 pixels to match the device's screen resolution.

> [!TIP]
> You can set an image as the sleep screen cover directly from the BMP image viewer in the **[Browse Files](#33-browse-files-screen)** screen.

---

### 3.8 Custom Fonts (SD Card)

CrossPoint supports loading additional fonts from the SD card, extending beyond the three built-in families (Noto Serif, Noto Sans, Open Dyslexic). Custom fonts can include extended Unicode coverage, enabling CJK (Chinese, Japanese, Korean) and other scripts.

There are three ways to install fonts:

1. **Download from device (recommended):** Go to **Settings → System → Manage Fonts**, browse the available font families, and select one to download over WiFi.
2. **Upload via web interface:** While in **File Transfer** mode, open the web UI in a browser and navigate to the **Fonts** tab to upload `.cpfont` files.
3. **Manual SD card copy:** Download font files from the [crosspoint-fonts repository](https://github.com/crosspoint-reader/crosspoint-fonts) and copy them to `/.fonts/` (preferred) or `/fonts/` on your SD card.

Once installed, custom fonts appear in **Settings → Reader → Font Family** alongside the built-in fonts.

See [docs/sd-card-fonts.md](./docs/sd-card-fonts.md) for full installation details and SD card folder structure.

---

## 4. Reading Mode

Once you have opened a book, the button layout changes to facilitate reading.

### Page Turning

| Action            | Buttons                              |
| ----------------- | ------------------------------------ |
| **Previous Page** | Press **Left** _or_ **Volume Up**    |
| **Next Page**     | Press **Right** _or_ **Volume Down** |

The role of the volume (side) buttons can be swapped in the **[Controls Settings](#363-controls)**.

If the **Short Power Button Click** setting is set to "Page Turn", you can also turn to the next page by briefly pressing the Power button.

### Chapter Navigation

* **Next Chapter:** Press and **hold** the **Right** (or **Volume Down**) button briefly, then release.
* **Previous Chapter:** Press and **hold** the **Left** (or **Volume Up**) button briefly, then release.

This feature can be disabled in the **[Controls Settings](#363-controls)** to help avoid changing chapters by mistake.

### Auto Page Turn

Auto Page Turn automatically advances pages at a set interval, useful for hands-free reading. This feature can be enabled and configured from the **[Reader Menu](#5-reader-menu)** while reading an EPUB.

### Tilt Page Turn (X3 only)

On the **Xteink X3**, the gyroscope can be used to turn pages by tilting the device. This feature is available in the Controls settings.

### Footnote Navigation

When reading an EPUB that contains footnotes, you can navigate to the footnote text by selecting the footnote reference in the book. From the footnote, you can return to your original reading position.

### System Navigation

* **Return to Home:** Press the **Back** button to close the book and return to the **[Home](#31-home-screen)** screen.
* **Return to Browse Files:** Press and hold the **Back** button to close the book and return to the **[Browse Files](#33-browse-files-screen)** screen.
* **Reader Menu:** Press **Confirm** to open the **[Reader Menu](#5-reader-menu)**, which includes chapter navigation, reading options, and more.

### Supported Languages

CrossPoint renders text using the following Unicode character blocks, enabling support for a wide range of languages:

* **Latin Script (Basic, Supplement, Extended-A/B):** Covers English, German, French, Spanish, Portuguese, Italian, Dutch, Swedish, Norwegian, Danish, Finnish, Polish, Czech, Hungarian, Romanian, Slovak, Slovenian, Turkish, Catalan, and others.
* **Cyrillic Script (Standard and Extended):** Covers Russian, Ukrainian, Belarusian, Bulgarian, Serbian, Macedonian, Kazakh, Kyrgyz, Mongolian, and others.
* **Vietnamese:** Supported via extended Latin glyph coverage in the built-in fonts.

What is not supported with built-in fonts: Chinese, Japanese, Korean, Hebrew, Arabic, Greek, and Farsi. However, **CJK and other extended scripts can be enabled by installing custom SD card fonts** — see [Custom Fonts (SD Card)](#38-custom-fonts-sd-card).

---

## 5. Reader Menu

Press **Confirm** while reading to open the Reader Menu. From here you can access reading utilities and navigation options without leaving the book.

Available options include:

- **Select Chapter** – Open the table of contents to jump to a specific chapter (see [Chapter Selection](#51-chapter-selection) below).
- **Footnotes** – Navigate to the footnotes for the current section *(only shown in books that contain footnotes)*.
- **Reading Orientation** – Cycle through screen orientations without leaving the reader.
- **Auto Turn (Pages Per Minute)** – Cycle through automatic page turn speed options for hands-free reading.
- **Go to %** – Jump to a specific position in the book by percentage.
- **Take screenshot** – Save a screenshot of the current page to the `screenshots/` folder.
- **Show page as QR** – Display a QR code encoding the current reading position.
- **Go Home** – Close the book and return to the Home screen.
- **Sync Progress** – Push or pull reading progress with a KOReader sync server (see [KOReader Sync Quick Setup](#367-koreader-sync-quick-setup)).
- **Delete Book Cache** – Clear the cached layout data for the current book, forcing a re-index on next open.

Press **Back** at any time to close the menu and return to your current page.

### 5.1 Chapter Selection

Accessible by selecting **Chapters** from the Reader Menu.

1. Use **Left** (or **Volume Up**), or **Right** (or **Volume Down**) to highlight the desired chapter.
2. Press **Confirm** to jump to that chapter.
3. *Alternatively, press **Back** to cancel and return to your current page.*

---

### 5.2 Bookmarks

Bookmarks can be created to quickly save and restore your place in a book.

To create a bookmark, hold **Confirm** for 1 second while inside a book. A popup will appear letting you know a bookmark was created. The popup message will automatically disappear in a couple of seconds.

To open bookmarks, press **Confirm** while inside a book. Then navigate to the **Bookmarks** menu. Bookmarks can be opened by navigating to them and pressing **Confirm**, which will redirect you to that place in the book. You can delete bookmarks by holding **Confirm** for 1 second, and then pressing **Confirm** again to confirm deletion, or **Back** to cancel.

Bookmarks are stored in the `.crosspoint/bookmarks` folder in the JSON format.

## 6. Current Limitations & Roadmap

Please note that this firmware is currently in active development. The following features are **not yet supported** but are planned for future updates:

* **Cover Images:** Large cover images embedded into EPUB require several seconds (~10s for ~2000 pixel tall image) to convert for sleep screen and home screen thumbnail. Consider optimizing the EPUB with e.g. https://github.com/bigbag/epub-to-xtc-converter to speed this up.
* **Unsupported Image Formats:** Most JPG and PNG images in EPUBs render correctly. GIFs and progressive JPEGs are not supported and will fall back to an `[Image]` placeholder.
* 
* **Dictionary Lookup:** Inline word lookup is not yet implemented.

---

## 7. Troubleshooting Issues & Escaping Bootloop

If an issue or crash is encountered while using Crosspoint, feel free to raise an issue ticket and attach the logs.

**Crash reports on SD card:** After a crash, CrossPoint automatically saves a crash report to the SD card (no USB connection needed). Check the root of the SD card for a crash log file and include it with any bug report.

**Serial monitor logs:** For more detailed debugging, connect the device to a computer and run the custom debugging monitor script (requires Python 3 with `pyserial`, `colorama`, and `matplotlib`; install via `pip3 install pyserial colorama matplotlib`):

```
python3 scripts/debugging_monitor.py
```

The script auto-detects the serial port. You can also specify one explicitly:

```
python3 scripts/debugging_monitor.py /dev/ttyACM0        # Linux
python3 scripts/debugging_monitor.py /dev/tty.usbmodem1  # macOS
python3 scripts/debugging_monitor.py COM7                # Windows
```

**Features:**

- Color-coded log output by category (errors, memory, display, EPUB parsing, etc.)
- Live memory usage graph (free RAM, total RAM, max contiguous allocation) updated every second
- Interactive command prompt — type a command and press Enter to send it to the device
- Screenshot capture — saves the current display to `screenshot.bmp` when triggered by the device

**Options:**

| Option               | Description                                               |
| -------------------- | --------------------------------------------------------- |
| `--baud RATE`        | Baud rate (default: 115200)                               |
| `--filter KEYWORD`   | Show only lines containing the keyword (case-insensitive) |
| `--suppress KEYWORD` | Hide lines containing the keyword (case-insensitive)      |

**Examples:**

```
# Show only memory-related log lines
python3 scripts/debugging_monitor.py --filter MEM

# Hide noisy SD card log lines
python3 scripts/debugging_monitor.py --suppress "[SD]"
```

Press **Ctrl-C** or close the graph window to exit.

If the device is stuck in a bootloop, press and release the Reset button. Then, press and hold on to the configured Back button and the Power Button to boot to the Home Screen.

There can be issues with broken cache or config. In this case, delete the `.crosspoint` directory on your SD card (or consider deleting only `settings.bin`, `state.bin`, or `epub_*` cache directories in the `.crosspoint/` folder).

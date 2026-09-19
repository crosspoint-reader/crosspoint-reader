# CrossPoint User Guide

Use this guide to set up the reader, add books, and read. Start with the sections that you need. You do not need a computer for daily reading.

Contents:

- [Buttons and screen](#1-buttons-and-screen)
- [Turn the reader on and off](#2-turn-the-reader-on-and-off)
- [Main screens](#3-main-screens)
- [Reading Mode](#4-reading-mode)
- [Reader Menu](#5-reader-menu)
- [Limits](#6-limits)
- [If something goes wrong](#7-if-something-goes-wrong)

## 1. Buttons and screen

The Xteink X4 uses these buttons by default. You can change the bottom-button layout in **Controls**.

### Button Layout

| Location        | Buttons                                              |
| --------------- | ---------------------------------------------------- |
| **Bottom Edge** | **Back**, **Confirm**, **Left**, **Right**           |
| **Right Side**  | **Power**, **Side Up**, **Side Down**, **Reset** |

The front buttons can have different jobs if you change them in **[Controls Settings](#363-controls)**.

### Taking a Screenshot

Press **Power** and **Side Down** together to save a picture of the screen. CrossPoint saves it in `screenshots/`.

While you read, you can also press **Confirm** and select **Take screenshot**.

### Frontlight (X4 Pro only)

The X4 Pro has a frontlight. You change it from a panel at the top of the screen.

* Swipe down from the top edge. Move the brightness and warmth sliders. Tap the sun to turn the light on or off.
* Press **Power** twice to turn the frontlight on or off without opening the panel.

> [!NOTE]
> Change frontlight brightness and warmth only in the swipe panel.

If the frontlight stays off after sleep, turn on **Restore Light on Wake** in **[Display Settings](#361-display)**.

---

## 2. Turn the reader on and off

### Power On / Off

Hold **Power** for about half a second to turn the reader on or off. You can set a short press to turn it off in **[Controls Settings](#363-controls)**.

If the reader freezes, press and release **Reset**. Then quickly hold **Power** for a few seconds.

### First Launch

When you turn on a new reader, it opens the **[Home](#31-home-screen)** screen.

> [!NOTE]
> Later, the reader opens the last book that you read.

---

## 3. Main screens

### 3.1 Home Screen

Home is the main screen. From it, you can resume reading, browse files, open the Library, transfer files, or open Settings.

### 3.2 Reading Mode

See [Reading Mode](#4-reading-mode) for the controls that you use in a book.

### 3.3 Browse Files Screen

Browse Files lets you find books and folders on the SD card. The current folder appears at the top. Folders appear in brackets, such as `[books]`.

* Use **Left** or **Side Up** to move up. Use **Right** or **Side Down** to move down. Hold a button to move a full screen at a time.
* Press **Confirm** to open a folder or read a book. A `.bmp` file opens as a picture.
* Hold and release **Confirm** to delete the selected file or folder. Confirm the choice when asked.
* You can rename or move a file from this screen.

### 3.4 Library Screen

The Library lists up to 4,096 supported books from the SD card. It shows each title and author, so you do not need to remember the folder. Its four tabs give different views. An arrow shows the order:

- **Recent** lists the ten books that you opened most recently. Hold a book to remove it from this list.
- **Added** shows books in the order that the Library first found them. Down shows the newest books first.
- **Title** groups books by the first letter of the title. Titles that start with a number or punctuation appear under `#`.
- **Author** groups books by author.

On a button-only device:

- Use **Up/Down** or **Left/Right** to move one row at a time. Hold a direction to move a page at a time.
- Press **Confirm** to open the selected book.
- Press **Back** from the book list to focus the tabs. Use **Left/Right** to select another tab, press **Confirm** to reverse its sort direction, or press **Down** to return to the list.
- While the tabs are focused, hold **Confirm** to open Search.
- In the Title or Author views, hold **Confirm** on a book to collapse the list to its letter or author groups. The matching group remains selected. Press **Confirm** to enter a group, or **Back** to restore the exact book and position you came from.

On a touch device, tap tabs, books, and Search. Tap an active tab again to reverse its order. Swipe to scroll. Hold a book in **Recent** to remove it. Hold a book in **Title** or **Author** to show its groups.

The Library prepares its list when you first open it. If you add books later, use **Settings → System → Rebuild library index**. **Use book metadata** lets the Library use the title and author saved inside a book.

### 3.5 File Transfer Screen

File Transfer lets you move files over Wi-Fi. Select **Join a Network**, **Calibre Wireless**, or **Create Hotspot**. The reader then shows an address that you open in a browser.

See [Transfer files over Wi-Fi](./docs/webserver.md) for the full steps.

You can upload, download, move, rename, and delete files from the browser.

You can download books and screenshots from the reader without a cable.

A Wi-Fi strength symbol appears while the reader uses a joined network.

> [!TIP]
> If an EPUB does not display correctly, run **EPUB Optimizer** on the reader. It prepares the book again for CrossPoint.

### 3.5.1 Send books from Calibre

You can send books from Calibre with the CrossPoint Reader plugin.

#### Install the Calibre plugin

If the plugin is not installed:

1. Go to https://github.com/crosspoint-reader/calibre-plugins/releases.
2. Download the latest `crosspoint_reader` zip file.
3. In Calibre, open **Preferences → Plugins → Load plugin from file**. Select the zip file.
4. Restart Calibre.

#### Set up the Calibre plugin

1. In Calibre, open **Preferences → Plugins**.
2. Search for `crosspoint`.
3. Select **Customize plugin**.
4. Set **Host** to the address shown on the reader.
5. Leave the other settings unchanged.
6. If you want books in a folder, set **Upload path** to a path such as `/mybooks`.
8. Restart Calibre.

<img width="420" height="385" alt="Image" src="https://github.com/user-attachments/assets/01fc7e33-a9a7-48ba-9e26-2e68d1f9daec" />

#### Send books

To upload a book using the CrossPoint plugin in Calibre:

1. On the reader, open **File Transfer → Calibre Wireless**. Then join a network.
2. Select one or more books.
3. Right-click on that selection.
4. Select **Send to Device → Send to main memory**.

The plugin connects to the reader. It creates an author folder and copies each book into it. If you set an upload folder, it uses that folder instead.

<img width="783" height="310" alt="Image" src="https://github.com/user-attachments/assets/741b0909-2e1d-4f16-8af0-2c43fbda5ce6" />

#### Remove books

You cannot remove books through Calibre. Use the file-transfer page instead.

### 3.6 Settings

Settings changes how the reader looks and works. This section explains the available choices.

#### 3.6.1 Display

- **Sleep Screen**: Choose what the reader shows when it sleeps:
  
  - **Dark** is the CrossPoint logo on a dark background.
  - **Light** is the CrossPoint logo on a white background.
  - **Custom** uses an image from the SD card. See [Sleep Screen](#37-sleep-screen).
  - **Cover** uses the open book cover. This feature is experimental.
  - **None** shows a blank screen.
  - **Cover + Custom** uses the cover while you read. At other times, it uses the custom image.
  - **Quick resume** keeps the last page on screen. It returns to that page quickly after sleep.
  - **Transparent** puts an overlay image on the current screen. See [Sleep Screen](#37-sleep-screen).
- **Sleep Screen Cover Mode**: How to display the book cover when "Cover" sleep screen is selected:
  
  - "Fit" (default) - Scale the image down to fit centered on the screen, padding with white borders as necessary
  - "Crop" - Fill the screen and cut off part of the cover. This feature is experimental.

- **Sleep Screen Cover Filter**: What filter will be applied to the book cover when "Cover" sleep screen is selected:
  
  - "None" (default) - The cover image will be converted to a grayscale image and displayed as it is
  - "Contrast" - The image will be displayed as a black & white image without grayscale conversion
  - "Inverted" - The image will be inverted as in white & black and will be displayed without grayscale conversion

- **Quick Resume on Timeout**: Use Quick resume when the reader sleeps after inactivity. It replaces the normal sleep-screen choice.

- **Status Bar**: Configure the status bar displayed while reading:
  
  - "None" - No status bar
  - "No Progress" - Show status bar without reading progress
  - "Full w/ Percentage" - Show status bar with book progress (as percentage)
  - "Full w/ Book Bar" - Show status bar with book progress (as bar)
  - "Book Bar Only" - Show book progress (as bar)
  - "Full w/ Chapter Bar" - Show status bar with chapter progress (as bar)

- **Hide Battery %**: Choose where to hide the battery number. The battery symbol remains visible:
  
  - "Never" (default) - Always show battery percentage
  - "In Reader" - Show battery percentage everywhere except in reading mode
  - "Always" - Always hide battery percentage

- **Refresh Frequency**: Choose how often the screen fully refreshes while you read. A full refresh removes faint traces from an earlier page.

- **UI Theme**: Set which UI theme to use:
  
  - "Classic" - The original Crosspoint theme
  - "Lyra" - The new theme for Crosspoint featuring rounded elements and menu icons
  - "Lyra Extended" - Lyra, but displays 3 books instead of 1 on the **[Home Screen](#31-home-screen)**
  - "RoundedRaff" - A rounded theme with additional visual styling

- **Sunlight Fading Fix**: Turn this on if a white X4 screen fades in direct sunlight:
  
  - "OFF" (default) - Disable the fix
  - "ON" - Enable the fix

> [!NOTE]
> A battery charging indicator is shown on the battery icon whenever the device is actively charging.

#### 3.6.2 Reader

- **Reader Font Family**: Choose the typeface for book text:
  
  - "Noto Serif" (default) - Google's serif font
  - "Noto Sans" - Google's sans-serif font

- **Reader Font Size**: Choose **Small**, **Medium**, **Large**, or **X Large** text.

- **Reader Line Spacing**: Choose **Tight**, **Normal**, or **Wide** space between lines.

- **Reader Screen Margin**: Choose the empty space around book text.

- **Reader Paragraph Alignment**: Choose **Justified**, **Left**, **Center**, or **Right** text.

- **Embedded Style**: Choose whether CrossPoint uses the formatting saved inside an EPUB.

- **Hyphenation**: Choose whether CrossPoint splits a long word at the end of a line.

- **Reading Orientation**: Set the screen orientation for reading EPUB files:
  
  - "Portrait" (default) - Standard portrait orientation
  - "Landscape CW" - Landscape, rotated clockwise
  - "Inverted" - Portrait, upside down
  - "Landscape CCW" - Landscape, rotated counter-clockwise

- **Extra Paragraph Spacing**: Set how to handle paragraph breaks:
  
  - "ON" - Vertical space will be added between paragraphs in Reading Mode
  - "OFF" - Paragraphs will not have vertical space added, but will have first-line indentation

- **Dictionary**: Select a StarDict dictionary, or select **None** to turn off lookups. This setting appears after you add a dictionary to `/dictionaries/`. See [Dictionary setup](docs/dictionary.md).

- **Text Anti-Aliasing**: Show smooth gray edges on text. Page turns can be slower.

- **Images**: Choose whether to show pictures inside EPUB files.

- **Focus Reading**: Makes the first part of each word bold. See [Focus Reading](docs/focus-reading.md).

#### 3.6.3 Controls

- **Remap Front Buttons**: Change what each bottom button does.

- **Side Button Layout (reader)**: Swap the side buttons for previous and next page, or turn them off while you read.

- **Long-press Chapter Skip**: Choose what happens when you hold a page-turn button:
  
  - "Chapter Skip" (default) - Long-pressing skips to next/previous chapter
  - "Page Scroll" - Long-pressing scrolls a page up/down
- **Long-press Menu**: Choose what happens when you hold **Confirm** in an EPUB. A short press always opens the reader menu:
  - "Bookmark" (default) - Hold Confirm (~0.4 second) to drop a bookmark at the current page.
  - "KOSync" - Hold Confirm (~1 second) to launch KOReader sync directly.
  - "Dictionary" - Hold Confirm (~0.4 second) to start dictionary word selection on the current page (see [docs/dictionary.md](docs/dictionary.md)).
  - "Disabled" - Holding **Confirm** does nothing. A short press opens the reader menu.

- **Short Power Button Click**: Controls the effect of a short click of the power button:
  
  - "Ignore" (default) - Require a long press to turn off the device
  - "Sleep" - A short press puts the device into sleep mode
  - "Page Turn" - A short press moves to the next page. A long press turns the reader off.
  - "Footnotes" - A short press opens footnotes. If a page has one footnote, it opens directly. You can use **Power** to return.
  - "Refresh" - A short press triggers a manual full-screen refresh, useful for clearing ghosting
- **Quick-return from footnotes**: When this is on, a short press of **Power** returns from a footnote.

#### 3.6.4 System

- **Time to Sleep**: Choose how long the reader waits without use before it sleeps.

- **Wi-Fi Networks**: Connect to Wi-Fi for file transfer and firmware updates.

- **KOReader Sync**: Set up progress sync with compatible KOReader readers and apps. **Smart sync** chooses the newer reading position in simple cases. Choose **Ask every time** if you want to decide each time.

- **OPDS Servers**: Manage online book catalogs for browsing and downloading books. See [Book catalogs](#365-book-catalogs).

- **Clear Reading Cache**: Remove saved book layout information from the SD card. CrossPoint creates it again when you open a book.

- **Use book metadata**: Use the title and author stored inside a book when the Library rebuilds its list. Otherwise, it uses the file name.

- **Rebuild library index**: Look for books on the SD card again.

- **Check for updates**: Look for CrossPoint updates over Wi-Fi. You can also put `firmware.bin` on the SD card to update without USB.

- **Language**: Set the UI language. CrossPoint supports 32 languages: English, Spanish, French, German, Czech, Brazilian Portuguese, European Portuguese, Russian, Swedish, Romanian, Catalan, Ukrainian, Belarusian, Italian, Polish, Finnish, Danish, Dutch, Turkish, Kazakh, Hungarian, Lithuanian, Slovenian, Valencian, Hebrew, Arabic, Slovak, Bosnian, Vietnamese, Norwegian Bokmål, Indonesian, and Orangutan.

- **Manage Fonts**: Browse, download, and manage custom font families installed from the SD card. See [Custom Fonts (SD Card)](#38-custom-fonts-sd-card) for more information.

#### 3.6.5 Book catalogs

An OPDS catalog is an online list of books. CrossPoint can save up to eight catalogs and switch between them.

1. Open **Settings -> System -> OPDS Servers**.

2. Select **Add Server** to create a new entry, or select an existing server to edit it.

3. Enter the information from your catalog:
   
   - **Server Name**: An optional name, such as "Home Calibre".
   
   - **OPDS Server URL**: The full catalog address. A Calibre catalog usually ends in `/opds`.
   
   - **Username / Password**: Login details, if the catalog needs them.

4. Use **Delete Server** inside a server entry to remove it.

You can also manage book catalogs from the file-transfer page:

1. Connect to the device web UI.
2. Open `http://<device-ip>/settings`.
3. Use the **OPDS Servers** card to add, edit, or delete entries.

For Wi-Fi management in a browser, see [Web Settings](#366-web-settings-wi-fi--opds).

#### 3.6.6 Web Settings

The file-transfer settings page lets you manage saved Wi-Fi networks and book catalogs.

1. On device: open **File Transfer** and connect through **Join a Network** or **Create Hotspot**.
2. In a browser, open `http://<device-ip>/settings` or `http://crosspoint.local`.
3. In **Wi-Fi Networks**, add, edit, or delete saved networks.
4. In **OPDS Servers**, add, edit, or delete book catalogs.

Behavior notes:

- The page never shows a saved password.
- If you leave Password blank while editing, CrossPoint keeps the saved password.

#### 3.6.7 Sync your reading progress

CrossPoint can share your reading position with compatible KOReader apps and devices. Each device must use the same sync service, username, and password.

##### Use the CrossPoint sync service

If you leave **Sync Server URL** empty, CrossPoint uses the free service at `https://sync.crosspointreader.com`. Compatible KOReader apps can use the same service.

1. On each CrossPoint device:

   - Go to **Settings -> System -> KOReader Sync**.

   - Set the same **Username** and **Password** on every device.

   - Leave **Sync Server URL** empty (or set it to `https://sync.crosspointreader.com`).

   - On the first device, select **Sign Up**. On every other device, select **Authenticate**.

Accounts belong to one service. If you already use `sync.koreader.rocks`, use the next option or create an account again.

##### Use an existing KOReader service

Use this option if you already sync KOReader devices with `sync.koreader.rocks`.

1. On each CrossPoint device:

   - Go to **Settings -> System -> KOReader Sync**.

   - Set **Sync Server URL** to `https://sync.koreader.rocks`.

   - Set **Username** and **Password** to your existing KOReader Sync credentials.

   - Run **Authenticate**.

2. If you do not have an account, select **Sign Up** on the device. The command below is for advanced users only:

```bash
USERNAME="user"
PASSWORD="pass"
PASSWORD_MD5="$(printf '%s' "$PASSWORD" | openssl md5 | awk '{print $2}')"

curl -i "https://sync.koreader.rocks/users/create" \
  -H "Accept: application/vnd.koreader.v1+json" \
  -H "Content-Type: application/json" \
  --data "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD_MD5\"}"
```

When this returns `HTTP 402` with `{"code":2002,"message":"Username is already registered."}`, pick a different username or use that existing account.

##### Advanced: run your own sync service

This option needs server and command-line experience. Use the CrossPoint service unless you need your own service.

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

2. Test the server:

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
   
   - Set the same **Username** and **Password** on every device.
   
   - Set **Sync Server URL** to `http://<server-ip>:17200`.
   
   - Run **Authenticate**.

If you use the HTTPS listener, use `https://<server-ip>:7200` (`curl -k` only for self-signed certificate testing).

##### Sync while you read

After setup, press **Confirm** while you read and select **Sync Progress**. You can also set **Settings → Controls → Long-press Menu** to **KOSync**.

- With **Ask every time**, select **Apply Remote** to use the other device position. Select **Upload Local** to use this reader position.
- With **Smart sync**, CrossPoint chooses the farther reading position in simple cases.

### 3.7 Sleep Screen

**Sleep Screen** controls what the reader shows when it sleeps:

| Mode               | Behavior                                                                                                                     |
| ------------------ | ---------------------------------------------------------------------------------------------------------------------------- |
| **Dark** (default) | The CrossPoint logo on a dark background. |
| **Light** | The CrossPoint logo on a white background. |
| **Custom** | An image from the SD card. It uses **Dark** if no image is available. |
| **Cover** | The open book cover. It uses **Dark** if no book is open. |
| **Cover + Custom** | The book cover while you read. At other times, it uses the custom image. |
| **Quick resume** | The last page stays on screen and opens quickly after sleep. |
| **Transparent** | A BMP or PNG image over the current screen. It uses **Dark** if no image is available. |
| **None** | A blank screen. |

#### Cover settings

When you use **Cover** or **Cover + Custom**, you can also choose:

- **Sleep Screen Cover Mode**: **Fit** shows the whole cover with white borders. **Crop** fills the screen and cuts off part of the cover.
- **Sleep Screen Cover Filter**: **None** uses gray tones. **Contrast** uses black and white. **Inverted** reverses black and white.

#### Custom images

To use your own sleep images, select **Custom** or **Cover + Custom**. Then add images to the SD card:

- **Several images:** Create `.sleep` in the main SD card folder. Put `.bmp` files inside. CrossPoint picks one at random when it sleeps. A folder named `sleep` also works.
- **One image:** Put `sleep.bmp` in the main SD card folder. CrossPoint uses it before any image in the folders.

#### Transparent overlay images

To use an overlay image, select **Transparent**. Then add BMP or PNG files to the SD card:

- **Several images:** Create `.sleep-overlay` in the main SD card folder. Put `.bmp` or `.png` files inside. CrossPoint picks one at random. A folder named `sleep-overlay` also works.
- **One image:** Put `sleep-overlay.bmp` or `sleep-overlay.png` in the main SD card folder. A BMP takes priority over a PNG and both take priority over the folders.

Overlay files stay separate from normal sleep images. In a regular BMP, white areas leave the screen unchanged. Use a PNG with transparency or a 32-bit BGRA BMP for partly transparent areas.

> [!TIP]
> For **Custom**, use an uncompressed 24-bit BMP. For **Transparent**, use a PNG or uncompressed 32-bit BGRA BMP. Use 480x800 pixels for X4 or 528x792 pixels for X3.

> [!TIP]
> You can set an image as the sleep screen cover directly from the BMP image viewer in the **[Browse Files](#33-browse-files-screen)** screen.

---

### 3.8 Custom Fonts (SD Card)

You can add fonts from the SD card. This lets you read Chinese, Japanese, Korean, and other scripts in books.

There are three ways to install fonts:

1. **Download on the reader:** Open **Settings → System → Manage Fonts** and select a family.
2. **Upload from a browser:** Open File Transfer, then upload `.cpfont` files in **Fonts**.
3. **Copy to the SD card:** Download files from the [crosspoint-fonts repository](https://github.com/crosspoint-reader/crosspoint-fonts). Copy them to `/.fonts/` or `/fonts/`.

The new font appears in **Settings → Reader → Font Family**.

See [docs/sd-card-fonts.md](./docs/sd-card-fonts.md) for full installation details and SD card folder structure.

---

## 4. Reading Mode

When you open a book, the buttons help you read.

### Page Turning

| Action            | Buttons                              |
| ----------------- | ------------------------------------ |
| **Previous Page** | Press **Left** _or_ **Side Up**    |
| **Next Page**     | Press **Right** _or_ **Side Down** |

You can swap the side buttons in **[Controls Settings](#363-controls)**.

If **Short Power Button Click** is **Page Turn**, a short press of **Power** moves to the next page.

### Chapter Navigation

* **Next Chapter:** Press and **hold** the **Right** (or **Side Down**) button briefly, then release.
* **Previous Chapter:** Press and **hold** the **Left** (or **Side Up**) button briefly, then release.

You can turn this off in **[Controls Settings](#363-controls)**.

### Auto Page Turn

Auto Page Turn moves to the next page after a chosen time. Turn it on in the **[Reader Menu](#5-reader-menu)** while you read an EPUB.

### Tilt Page Turn (X3 only)

On the Xteink X3, you can turn pages by tilting the reader. Turn this on in Controls.

### Footnote Navigation

If an EPUB contains footnotes, select a footnote link to open it. You can then return to where you were reading.

If the reader sleeps or you close the book from a footnote, it returns to your earlier reading position.

### Dictionary Lookup

Copy a StarDict dictionary to `/dictionaries/` on the SD card. Select it in **Settings → Reader → Dictionary**. Then select **Look Up** in the **[Reader Menu](#5-reader-menu)**. You can instead set **Long-press Menu** to **Dictionary** and hold **Confirm**. Use **Left** and **Right** to choose a word. Press **Confirm** to read its definition.

See [Dictionary setup](docs/dictionary.md) to find and add a dictionary.

### System Navigation

* **Return to Home:** Press **Back** to close the book and return to **[Home](#31-home-screen)**.
* **Return to Browse Files:** Hold **Back** to close the book and return to **[Browse Files](#33-browse-files-screen)**.
* **Reader Menu:** Press **Confirm** to open the **[Reader Menu](#5-reader-menu)**.
* **Hold Confirm:** Run the action selected in **Long-press Menu**. **Bookmark** is the default. You can choose **KOSync**, **Dictionary**, or **Disabled** instead.

### Supported Languages

The built-in book fonts support these languages:

* **Latin:** English and many European languages.
* **Cyrillic:** Russian, Ukrainian, Belarusian, Bulgarian, Serbian, and others.
* **Vietnamese.**

The menus include Arabic and Hebrew. For book text in Chinese, Japanese, Korean, Arabic, Greek, Hebrew, Farsi, or another extended script, install an SD card font. See [Custom Fonts](#38-custom-fonts-sd-card).

---

## 5. Reader Menu

Press **Confirm** while you read to open the Reader Menu. It gives you reading tools without closing the book.

Available options include:

- **Select Chapter**: Open the table of contents and select a chapter.
- **Footnotes**: Open footnotes in this part of the book.
- **Look Up**: Look up a selected word. You must first select a dictionary in **Settings → Reader → Dictionary**.
- **Reading Orientation**: Rotate the reading screen.
- **Auto Turn**: Choose the automatic page-turn speed.
- **Go to %**: Jump to a percentage in the book.
- **Take screenshot**: Save the current page in `screenshots/`.
- **Show page as QR**: Show a QR code for the current position.
- **Go Home**: Close the book and return to Home.
- **Sync Progress**: Share your reading position. See [Sync your reading progress](#367-sync-your-reading-progress).
- **Delete Book Cache**: Remove saved layout information for this book. CrossPoint creates it again when you open the book.

Press **Back** at any time to close the menu and return to your current page.

### 5.1 Chapter Selection

Select **Chapters** from the Reader Menu.

1. Use **Left** or **Side Up** to choose a chapter above. Use **Right** or **Side Down** to choose one below.
2. Press **Confirm** to jump to that chapter.
3. Press **Back** to return to your current page.

---

### 5.2 Bookmarks

Bookmarks save your place in a book.

Hold **Confirm** for about half a second to add a bookmark. CrossPoint shows a short message.

To open a bookmark, press **Confirm** and select **Bookmarks**. Select a bookmark and press **Confirm**. To delete one, hold **Confirm** for about 0.7 seconds. Then press **Confirm** to delete it or **Back** to cancel.

CrossPoint saves bookmarks on the SD card.

## 6. Limits

Large EPUB cover images can take several seconds to prepare for the Home or sleep screen. Most JPG and PNG images in EPUBs work. GIF images and progressive JPEG images appear as `[Image]` instead.

---

<a id="7-troubleshooting-issues--escaping-bootloop"></a>

## 7. If something goes wrong

For file-transfer problems, see [Troubleshooting file transfer](docs/troubleshooting.md).

If CrossPoint crashes, it saves a crash report in the main folder of the SD card. Attach that file when you report the problem.

If the reader repeatedly restarts, press and release **Reset**. Then hold the configured **Back** button and **Power** to open Home.

If a book does not open correctly, remove the `.crosspoint` folder from the SD card. This removes saved book information and settings. CrossPoint creates the book information again, but you must set your preferences again. If you want to keep your preferences, remove only the `epub_*` folders inside `.crosspoint`.

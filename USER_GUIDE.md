# Witch Reader User Guide

Welcome to **Witch Reader** firmware. This guide outlines the hardware controls, navigation, and reading features of the device.

- [Witch Reader User Guide](#witch-reader-user-guide)
  - [1. Hardware Overview](#1-hardware-overview)
    - [Button Layout](#button-layout)
    - [Moving through lists](#moving-through-lists)
  - [2. Power \& Startup](#2-power--startup)
    - [Power On / Off](#power-on--off)
    - [First Launch](#first-launch)
  - [3. Screens](#3-screens)
    - [3.1 Home Screen](#31-home-screen)
    - [3.2 Reading Mode](#32-reading-mode)
    - [3.3 Library Screen](#33-library-screen)
    - [3.4 Recent, New and Authors](#34-recent-new-and-authors)
    - [3.5 Book Info Screen](#35-book-info-screen)
    - [3.6 File Transfer Screen](#36-file-transfer-screen)
      - [3.6.1 Calibre Wireless Transfers](#361-calibre-wireless-transfers)
      - [3.6.2 USB Transfer (over the cable)](#362-usb-transfer-over-the-cable)
      - [3.6.3 USB Drive (X4 Pro and LilyGo T5 S3)](#363-usb-drive-x4-pro-and-lilygo-t5-s3)
      - [3.6.4 OPDS Catalogs](#364-opds-catalogs)
    - [3.7 Settings](#37-settings)
      - [3.7.1 Display](#371-display)
      - [3.7.2 Reader](#372-reader)
      - [3.7.3 Controls](#373-controls)
      - [3.7.4 System](#374-system)
      - [3.7.5 OPDS Servers (Multiple Libraries)](#375-opds-servers-multiple-libraries)
      - [3.7.6 Web Settings (WiFi + OPDS)](#376-web-settings-wifi--opds)
      - [3.7.7 KOReader Sync Quick Setup](#377-koreader-sync-quick-setup)
    - [3.8 Sleep Screen](#38-sleep-screen)
  - [4. Reading Mode](#4-reading-mode)
    - [Page Turning](#page-turning)
    - [Chapter Navigation](#chapter-navigation)
    - [System Navigation](#system-navigation)
    - [Supported Languages](#supported-languages)
  - [5. Touch Controls](#5-touch-controls)
    - [5.1 The two master switches](#51-the-two-master-switches)
    - [5.2 Menus, lists and everything outside a book](#52-menus-lists-and-everything-outside-a-book)
    - [5.3 The reading page: taps and holds](#53-the-reading-page-taps-and-holds)
    - [5.4 The reading page: swipes](#54-the-reading-page-swipes)
    - [5.5 Corners, and two fingers](#55-corners-and-two-fingers)
    - [5.6 Reading light](#56-reading-light)
    - [5.7 Per-device differences](#57-per-device-differences)
    - [5.8 Turning it off](#58-turning-it-off)
    - [5.9 If a gesture does nothing](#59-if-a-gesture-does-nothing)
  - [6. Chapter Selection Screen](#6-chapter-selection-screen)
  - [7. Current Limitations \& Roadmap](#7-current-limitations--roadmap)
  - [8. Troubleshooting Issues \& Escaping Bootloop](#8-troubleshooting-issues--escaping-bootloop)


## 1. Hardware Overview

Four devices are supported: the **Xteink X3** and **Xteink X4** (buttons only), and the
**Xteink X4 Pro** and **LilyGo T5 S3 Pro** (buttons and a touchscreen).

### Button Layout

On the **X3 and X4**, the firmware uses the manufacturer's own button layout by default:

| Location        | Buttons                                              |
| --------------- | ---------------------------------------------------- |
| **Bottom Edge** | **Back**, **Confirm**, **Left**, **Right**           |
| **Right Side**  | **Power**, **Volume Up**, **Volume Down**, **Reset** |

The **X4 Pro** and **LilyGo T5 S3** have far fewer keys, and make up the difference with the
touchscreen:

| Device | Physical keys | Back / Confirm |
| --- | --- | --- |
| **X4 Pro** | Up, Down, Power | Capacitive Home key — **tap** for Confirm, **hold** for Back |
| **LilyGo T5 S3** | Down, Power | Capacitive Home key — **tap** for Confirm, **hold** for Back |

The T5 S3 has no **Up** key at all. On lists that follow **[Moving through lists](#moving-through-lists)**
the side **Up** box stands in for it, and any list pages backward with a swipe — see
**[Touch Controls](#5-touch-controls)**.

Button layout can be customized in the **[Controls Settings](#373-controls)**.

On the touch devices everything can also be driven by finger — see
**[Touch Controls](#5-touch-controls)**. Touch never replaces a button: every key keeps its
function, and all touch behaviour can be switched off.

### Moving through lists

The firmware's lists are moving to one set of buttons, a screen at a time. So far these work this
way: the **OPDS catalog** and the **OPDS servers** with their settings, **Settings** and the
**reader menu**, the option lists and pickers they open (fonts, dictionaries, language, keyboard
layouts, long option lists such as the time zone), the menus (file options, Home's More, clock,
KOReader sync, weather settings, quick overrides), **File Transfer**'s choice of mode, **Customise
Status Bar**, the **font manager**, **Reading Stats**' book list, the weather city search results
and the screen shown when you finish a book. Other lists still behave as their own sections
describe.

| Button | Press | Hold |
| --- | --- | --- |
| **Up / Down** | Move one row; tap twice quickly to jump a page | Jump to the first / last row (in Settings and the reader menu: switch to the previous / next tab) |
| **Left / Right** | Move one row, or the action the screen shows | Page back / forward; keep holding to keep paging |
| **Confirm** | Open the selected row | The same as a press |
| **Back** | Go back | Return to the Home screen (see below) |

A page jump moves the list by a screenful and keeps the selection on the same line of the screen.
On the last screen it goes to the last row, and on the first screen to the first row.

The hints show which is which. **«** on the Left box and **»** on the Right box mean *hold to
page*. The word after the arrow is what a short press does: **« Up** / **» Down** where Left and
Right move the selection, or the screen's own action, such as **« Search** / **» Info**. A box
showing only **«** or **»** has no action for the selected row right now. A short press does
nothing there, and holding it still pages.

Holding **Back** never throws away a change. Settings saves before going Home, and the reader
menu closes the book as its **Go Home** item does, keeping the menu's changes. Inside a book, the
lists opened over it (quick overrides, the pickers in the reader menu) and a file's options menu
treat a held Back as a press.

A button action you set yourself under **Settings → Controls** comes first: give a long Left or
Right press an action of its own and holding it no longer pages, and a short-press action on the
page-turn keys (Up/Down) runs as well as the move.

On the **X4 Pro** and the **LilyGo T5 S3**, tap a hint box to press that button and hold the box
to hold it. A held **«** or **»** box pages once rather than over and over, so swipe to move through
a long list quickly. Lists that work this way always show the side boxes **Up** and **Down**, so the
T5 S3, which has no Up key, can always move up.

### Questions

When the reader asks before doing something (removing a book, clearing the cache, installing an
update, saving a Wi-Fi password), the question has two buttons: the answer that leaves things as
they are (**Cancel** or **No**) first, and the one that goes ahead (**Update**, **Clear**, **Yes**...)
second. Press the front button whose hint names your answer, or tap the answer on a touch screen.
**Back** always answers **Cancel**; on the X4 Pro and the T5 S3, so does holding the Home key.

A few questions have a third answer between the two, such as **Reset info** in a saved Wi-Fi
network's options (**Cancel**, **Reset info**, **Forget**). Their buttons are stacked one above the
other. **Back** answers **Cancel** and carries its hint, and the two front buttons answer the other
two.

### Taking a Screenshot
When the Power Button and Volume Down button are pressed at the same time, it will take a screenshot and save it in the folder `screenshots/`.

Alternatively, while reading a book, press the **Confirm** button to open the reader menu and select **Take screenshot**.

---

## 2. Power & Startup

### Power On / Off

To turn the device on or off, **press and hold the Power button for approximately half a second**.
In the **[Controls Settings](#373-controls)** you can configure the power button to turn the device off with a short press instead of a long one.

To reboot the device (for example after a firmware update or if it's frozen), press and release the Reset button, and then quickly press and hold the Power button for a few seconds.

### First Launch

Upon turning the device on for the first time, you will be placed on the **[Home](#31-home-screen)** screen.

> [!NOTE]
> On subsequent restarts, the firmware will automatically reopen the last book you were reading.

---

## 3. Screens

### 3.1 Home Screen

The Home screen is the main entry point to the firmware. It shows the most recently read book as a cover thumbnail and provides navigation to **[Reading Mode](#4-reading-mode)**, the **[Library](#33-library-screen)**, **Reading Stats**, the **[File Transfer](#36-file-transfer-screen)** screen, and **[Settings](#37-settings)**. Bookmarks and a weather panel appear as well once they have something to show.

Under each book's cover is a line of its reading history, such as *Read 3h 18m over 5 days - last 6m ago*. It counts the days you opened the book on, not the number of times you opened it. Each theme shows as much of it as it has room for: where the full line would not fit, as on the X3's Lyra Carousel or at the Large UI font size, a short form such as *3h 18m · 5d* sits in the cover's badge instead. The day count is left out until the device's clock has been set.

**Choosing what is on the Home screen:** **Settings → Display → Home screen** has one switch each for Library, Reading stats, Bookmarks, File transfer and Weather. Everything is on by default. Anything you switch off moves behind a **More** row above Settings, so it is still one step away. More only appears when something has been moved there. Settings itself cannot be switched off, because it is where you undo the choice.

**Correcting a book's title or author:** a metadata file next to a book with the same name (`Some Book.opf` beside `Some Book.epub`) takes precedence over the details inside the book. It works for every format the reader opens: EPUB, XTC, and TXT and Markdown, which have no details of their own. The **Details** button beside a book in the web interface's File Manager writes one for you, including the name the author is sorted under in the Library's **Authors** tab (**Sort author as**). When that file is added, changed or removed, over USB or from the web interface, the Library's Details and Covers views show the new details without the book having to be opened first, and so does the Home screen, for a book in its recent list.

### 3.2 Reading Mode

See [Reading Mode](#4-reading-mode) below for more information.

### 3.3 Library Screen

The Library is where your books are. A row of tabs under the header switches between four lists of them:

* **Books**: your folders and the books in them, as they are on the card.
* **Recent**: the books you opened last.
* **New**: the ten books added to the card most recently.
* **Authors**: everyone who wrote a book on the card; open one to see their books.

Hold **Up** or **Down** to go to the previous or next tab (their button hints read **Up / Tab** and **Down / Tab**), or tap a tab on a touch screen. While the Library is open each tab keeps your place in it, and the Library opens on the tab you left it on. **Back** at the top of a tab returns to the Home screen. The header names the folder you are in, or the author you opened, and otherwise reads **Library**.

#### Books tab

The Books tab is a full-featured browser for your books and the folders they are in. A folder with no book anywhere in it is not listed, so folders left behind by a sync or holding only covers do not get in the way; **All Files** still shows them, and so does the folder picker of **Move to folder**. A folder made with **New Folder** here opens straight away. It lists the books the reader can open (EPUB, XTC/XTCH, TXT and Markdown) and nothing else: a cover image or `.opf` file saved beside a book (see [sidecar files](docs/sidecar-files.md)) is not listed on its own, so a book downloaded from an OPDS catalogue shows up once. Images and every other file are in **Settings → System → Tools → All Files**.

* **Navigate List:** **Up** and **Down** move the selection one row; tap either twice quickly to jump a screenful. In a folder longer than one screen, **Left** and **Right** page back and forward a screenful at a time (their hints read **« Page** and **» / Options**). The Covers view moves differently; see [Book view](#book-view).
* **Open Selection:** Press **Confirm** to open a folder or read a selected book. If KOReader sync is set up, a long **Confirm** on an EPUB fetches your progress from the server before the book opens.
* **Options menu:** Hold **Right** (the page-forward button) to open the menu for the selected item. In a folder short enough to fit on one screen, a short press of Right opens it too. On a device without a Confirm key, such as the X4 Pro, **Confirm** opens the menu instead, and a tap on a row opens the item. The button hints always show which button does what.
  * The menu starts with the display options: **Sort By**, **Sort Direction**, **Show Hidden Files**, **Show File Extensions** and **Book view**.
  * For a book: **Open**, **Fetch Progress & Open** (an EPUB, when KOReader sync is set up), **Mark as read**, **Info** and **Delete Book Cache** (not for TXT and Markdown), **Remove**, **Move to folder**, **New Folder**, **Search** and **Search all folders**.
  * For a folder: **Open**, the display options, **Search**, **Search all folders**, **New Folder** and **Remove**, which deletes the folder and everything in it.

#### Book view

**Book view** in the options menu switches how books are shown:

* **Filenames** (the default) lists each book by its filename.
* **Details** shows each EPUB's title from its metadata, with the author and series underneath (for example "Thomas Mann · Werke #3"), and how far you have read on the right: a percentage, or **Finished**. An `.opf` metadata file beside the book overrides what is inside the book, as everywhere else. An XTC book shows the title and author from the XTC file. TXT and Markdown books carry no metadata of their own, so they keep their filename unless an `.opf` file beside them names them (the web interface's **Details** button writes one for any book).

* **Covers** shows the folder as a grid of covers, with the title and author under each. Each cover is shown whole, scaled to fit, never cropped. A finished book has a folded corner and one you are reading a bar along the bottom. A folder appears as a folder the size of a cover, with the number of books in it and in all the folders below it (up to "999+"; "..." while they are being counted). The grid holds as many covers as the screen has room for: two rows of two on the X3 and X4, three of three on the LilyGo T5S3. **Up**/**Down** move a row and **Left**/**Right** one cover, wrapping round at the ends, and the page follows the selection. On a touch screen a tap selects a cover, a second tap opens it, and a swipe turns the page. Hold **Right** for the options menu: its hint reads **Right / Options**.
  * A book whose cover has not been made yet shows a card with its title at first. The covers of the page on screen are then made one at a time, and each appears as soon as it is ready. Pressing a button pauses this, so the screen stays responsive. A large cover can take several seconds. A cover is made once and kept, and the other tabs and the "book finished" screen use the same ones. A book with no cover at all keeps its title card; if making a cover fails, it is tried again the next time you open the folder.

The first time a folder shows a book whose details have not been read yet, its row or card shows the filename for a moment while the book is read: every EPUB and XTC, and a TXT or Markdown book with an `.opf` file beside it. After a firmware update that changes what is kept, this happens once more for each book. The screen then redraws once with the details, and after that they come up at once. Sorting and **Search** still go by filename, and **Search all folders** lists its results by filename in every view.

#### Sorting

Files and folders can be sorted by **name**, **date**, **size**, or **type**, in either ascending or descending order. The sort order is set from the options menu.

#### Organising files

* **New Folder** makes a folder inside the one you are browsing. A name the SD card cannot hold is corrected rather than refused.
* **Move to folder** opens a folder picker that shows folders only. Browse to where the file should go: the move puts it in the folder you are *browsing*, which the header names in full, not in the row under the highlight. Press **Right** (the page-forward button; labelled **Move**) to move it there, or **Left** (the page-back button; labelled **New**) to make a new folder first. **Back** cancels.
* A move is instant whatever the size of the file, because nothing is copied. It never overwrites anything: if a file of the same name is already there, or the file is already in that folder, the move is refused and the reason shown.
* A book's cover image and `.opf` file go with it when you move or remove the book here, since this screen does not list them. In **All Files** each file is moved or removed on its own.
* **Remove** on a folder deletes it and everything in it. It sits last in the menu because it cannot be undone.

#### Searching

* **Search** narrows the folder you are in to the names that contain what you type (upper and lower case are treated the same). The search text is shown in the header.
* **Search all folders** searches this folder and every folder below it, and lists the matches with their path, so two books with the same name can be told apart. Opening a result opens it where it lives. **Go to folder** opens the folder a result is in, with the file selected.
* **Back** ends a search; so does changing folder. **Clear search** in the folder's menu does the same.
* The card is searched when you ask rather than indexed in advance, so results always reflect what is on the card, including anything copied over USB. **Show Hidden Files** applies to search as it does to browsing.

#### Large folders

Folders with many entries are handled via an SD-card-backed index so memory use stays bounded regardless of folder size.

### 3.4 Recent, New and Authors

The other three tabs list books from wherever they are on the card, with the same three views as the Books tab: **Covers**, **Details** and **Filenames**, chosen in the options menu under **Book view**. Each tab remembers its own choice. The keys are the Books tab's too: in Covers, **Up**/**Down** move a row and **Left**/**Right** one book, wrapping round at the ends; hold **Right** for the options menu. Selecting a book opens it at the last read position, and if KOReader sync is set up, a long **Confirm** on an EPUB fetches your progress first. Closing the book brings you back to the tab, and the row, you opened it from. Recent and New start in Covers, Authors in Details.

* **Recent** lists the books you opened last, newest first. **Remove from recents** in its options menu takes a book off the list; it stays on the card.
* **New** lists the ten books added to the card most recently, newest first. A book only moved to another folder does not count as new.
* **Authors** lists the authors of the books on the card surname first, as "Pratchett, Terry", each with the number of books by them. Open an author to see their books, by series and then title, under the author's name as the books spell it; **Back** returns to the authors. A book that names no author is under **Unknown author**.

New and Authors come from an index of the books on the card, kept on the card in `/.crosspoint/library/`. It is kept from one session to the next, and brought up to date when you open either tab after the card has changed: a book added or removed on the device, through the web interface, Calibre or an OPDS catalogue, or over USB. Books copied onto the card in a computer's card reader are noticed when the Books tab shows one of them or the folder they are in; anything it misses, **Refresh library** in the options menu of New or Authors picks up. While the index is brought up to date the header shows **Indexing**, the tab keeps showing what it had, and the lists fill in as it goes. A book that has never been opened has to be read once for its author, so the first time takes a while on a large card; books not read yet are under **Not yet indexed** until then. The author is the one an `.opf` metadata file beside the book names, if there is one, and that file's sort name ("file-as", such as "Pratchett, Terry") is how the author is listed. Without one, the last word of the name comes first. Up to 2,000 books are indexed.

The options menu on these tabs has what you can do with the selected book: open it, mark it as read, show its details, delete its cache, **Remove** (Recent: **Remove from recents**), and **Go to folder**, which opens the Books tab in the folder the book is in, with the book selected. On New and Authors it also has **Refresh library**, which builds the index again and reads every author anew. Its display options are **Show File Extensions** and **Book view**: sorting, hidden files and searching are on the Books tab only.

### 3.5 Book Info Screen

The Book Info screen shows full metadata for a book: cover image, title, author, description (paged if long), and reading statistics. Open it with **Info** in the Library's options menu, or from the reader menu while reading.

### 3.6 File Transfer Screen

The File Transfer screen allows you to upload new e-books to the device. When you enter the screen, you'll be prompted with a WiFi selection dialog and then your X4 will start hosting a web server.

The [Web Interface Guide](./docs/webserver.md) walks through every page of the web interface, with screenshots: connecting, the first-visit setup, uploading and organising books, the EPUB optimizer, book details, settings, fonts, dictionaries, reading stats, plugins and diagnostics.

The web interface's pages are kept on the SD card, not in the firmware. The first time you open the device's address, and after a firmware update that changed them, a setup page installs them: on your WiFi network it downloads them by itself; in hotspot mode, choose the `web-….bin` file from the release page, or copy it to the top folder of the SD card. The setup page can upload books on its own as well.

The **Dictionaries** page installs dictionaries for looking up words while you read. Choose the StarDict dictionary you downloaded, as an archive (`.zip`, `.tar.gz`, `.tar.bz2`) or as its files; your browser unpacks the parts the reader cannot use compressed, checks them and puts each dictionary into its own folder under `/dictionaries`. Then choose it on the device under **Settings → Reader → Dictionary**. The page also shows whether each installed dictionary is usable, and removes one. See [Dictionary](./docs/dictionary.md).

The **Diagnostics** page shows what **Boot Diagnostics** (under **Settings → System**) shows, in plain words: how the device last slept and started. **Download report** saves it, with the firmware version and device status, as one file to attach to a bug report, instead of a photo of the screen.

The web server has no password, so it keeps away from what it should not touch: the files holding your saved Wi-Fi, OPDS and KOReader passwords can never be downloaded or changed, and folders whose names start with a dot, such as the reader's own `/.crosspoint`, can be opened only when **Show Hidden Files** is on. **Back** leaves the screen even while the web page is uploading a book or a font; the upload is cancelled.

> [!TIP]
> Advanced users can also manage files programmatically or via the command line using `curl`. See [Webserver Endpoints](./docs/webserver-endpoints.md) for details.

### 3.6.1 Calibre Wireless Transfers

Witch Reader supports sending books from Calibre using the CrossPoint Reader device plugin.

1. Install the plugin in Calibre:
   - Head to https://github.com/crosspoint-reader/calibre-plugins/releases to download the latest version of the crosspoint_reader plugin.
   - Download the zip file.
   - Open Calibre → Preferences → Plugins → Load plugin from file → Select the zip file.
2. On the device: File Transfer → Connect to Calibre → Join a network.
3. Make sure your computer is on the same WiFi network.
4. In Calibre, click "Send to device" to transfer books.

### 3.6.2 USB Transfer (over the cable)

If you'd rather not use WiFi, you can transfer books over the USB cable. On the device choose **File Transfer → USB Transfer**; the screen then waits for a host connection and shows per-operation status (e.g. *Receiving 'book.epub'*, *Saved …*).

This uses a serial protocol that is wire-compatible with **[MicroReader](https://github.com/CidVonHighwind/microreader)** by CidVonHighwind — a clean-room, independently written reimplementation of its protocol — so MicroReader's host tools work unchanged:

- Its **Calibre device plugin** ("Send to device" over USB), and
- Its `tools/serial_cmd.py` command-line tool (`--upload`, `--list`, …).

A **Total Commander / Double Commander file-system plugin (WFX)** that works with both Witch Reader and MicroReader is available at **[jpirnay/x4-filemanager-plugin](https://github.com/jpirnay/x4-filemanager-plugin)**. It lets you browse and manage the SD card directly from the file manager over the USB cable.

Full credit to CidVonHighwind for the original protocol and tooling. While a transfer is in progress the device pauses on-screen redraws and on-wire logging so the binary stream stays clean.

**Open this screen first, then connect.** Opening the USB port from a computer briefly resets the device (a hardware quirk of the ESP32-C3's USB port). As long as you've opened the **USB Transfer** screen first, the device automatically returns to this screen after that reset, so the transfer just works — press **Back** when you're done. (If you connect while on another screen, the device will simply reboot to Home; open USB Transfer and reconnect.)

### 3.6.3 USB Drive (X4 Pro and LilyGo T5 S3)

On the **Xteink X4 Pro** and the **LilyGo T5 S3**, *File Transfer* offers **USB Drive** in place of USB Transfer. The reader appears on your computer as an ordinary USB stick, so you can copy books on and off — and reorganise folders, or clear caches — with your normal file manager. No plugin, no protocol, no WiFi.

Choose **File Transfer → USB Drive**, then connect the cable. The screen says *USB Drive Connected* once your computer has it mounted; give it up to half a minute the first time.

**When you're done, eject the drive on your computer** (or just unplug the cable). The reader then restarts by itself and returns to Home — that restart is deliberate, because your computer may have changed anything on the card and the reader has to re-read it. Nothing is lost: your reading position is saved before the drive starts.

While the drive is connected the reader is a disk and nothing else — buttons, sleep and the page you were reading are all suspended until you eject.

> These two boards can do this because their USB-C port is wired straight to the processor's own USB hardware. The X3 and X4 cannot: their ESP32-C3 has no USB device controller for it.

### 3.6.4 OPDS Catalogs

Once at least one OPDS server is set up (see [OPDS Servers](#375-opds-servers-multiple-libraries)), **File Transfer** lists **OPDS Browser**. It connects to WiFi by itself and opens the catalog; with more than one server, you pick the server first. Books you download are saved to the **Download folder** chosen under **Settings → System → OPDS Servers** (the card's root unless you change it). How to move through a catalog is described under [Browsing a catalog](#375-opds-servers-multiple-libraries).

**Back** from the server list or the top of the catalog returns to File Transfer, with **OPDS Browser** selected; hold **Back** to go Home instead. If the browser had connected to WiFi, the device restarts quietly on the way, as it does after the other WiFi modes. A catalog opened from a finished book's author search returns to Home.

### 3.7 Settings

The Settings screen allows you to configure the device's behavior.

The settings are grouped into tabs. The screen opens on the tab bar: **Confirm** moves to the next
tab, **Down** enters its list, and **Back** on a row returns to the bar (on the bar it saves and
returns Home). **Hold Up / Down** to switch to the previous / next tab: it opens where you left it.
Moving within a tab follows **[Moving through lists](#moving-through-lists)**.

#### 3.7.1 Display

- **Reading light** *(devices with a frontlight or backlight only)*: On/off, brightness, warmth (two-channel devices) and whether waking restores the light. See **[Reading light](#56-reading-light)**.
- **Screen Edge Margin**: How much clearance to leave between the text and the edge of the glass — "Narrow" (default, the existing behaviour), "Medium" (+5 px) or "Large" (+10 px). Useful where the case comes close to the live pixels, as on the LilyGo T5 S3. Pages re-flow when it changes.
- **Time to Sleep**: Slider from 0 (Never) to 60 minutes; sets the inactivity period before the device sleeps.
- **Sleep Screen**: Which sleep screen to display when the device sleeps:
  - "Dark" (default) - The Witch Reader logo on a dark background
  - "Light" - The same logo on a white background
  - "Custom" - Custom images from the SD card; see [Sleep Screen](#38-sleep-screen) for more information
  - "Cover" - The cover of the currently open book
  - "None" - A blank screen
  - "Cover + Custom" - Book cover with fallback to Custom behavior
  - "Page Overlay" - A transparent PNG composited over the current reader page (book content shows through)
  - "Quick Resume" - A minimal screen that resumes reading immediately on wake
- **Sleep Screen Cover Mode**: How to display the cover image:
  - "Fit" (default) - Scale to fit, white borders
  - "Crop" - Scale and crop to fill the screen
- **Sleep Screen Cover Filter**: Filter applied to the cover image:
  - "None" (default) - Grayscale
  - "Contrast" - Black & white without grayscale conversion
  - "Inverted" - Inverted black & white
  - "Adaptive" - Stretches the picture between its own darkest and lightest points
  - "Equalize" - Spreads the tones by how much of the picture carries them; stronger than Adaptive, and the better choice for a mostly dark cover with a small bright title
- **Sleep Screen Info Overlay**: When the device sleeps from a book, a band along the bottom of the sleep image with the book's title, author and progress (chapter, page and percentage, counted as the status bar counted them when you closed the book):
  - "Off" (default), or the band's colour: "White", "Gray", "Black"
- **Sleep Image Pick Mode**: How to cycle through images in the Custom sleep screen:
  - "Random" (default) - Pick a random image each time
  - "Sequential" - Cycle through images in order
- **Quick Resume Timeout**: Whether the Quick Resume sleep screen auto-clears on next wake.
- **Hide Battery %**: Where to suppress the battery percentage in the status bar:
  - "Never" (default), "In Reader", "Always"
- **Refresh Frequency** (submenu): Settings for screen refresh behaviour while reading:
  - **Refresh Frequency** - Slider (0 = Never, up to 60) for how often a full refresh runs to clear ghosting
  - **Refresh After Image Pages** - Whether to do an extra refresh after pages containing images
- **Sunlight Fading Fix**: Software fix for white X4 models that may fade in direct sunlight. "OFF" (default) / "ON".

**User interface**:
- **UI Theme**: Visual theme for the device UI:
  - "Classic" - The original theme
  - "Lyra" - Rounded elements and menu icons
  - "Lyra Extended" - Lyra with 3 books on the Home Screen
  - "Lyra Carousel" - Lyra with a full cover carousel on the Home Screen
- **UI Font Size**: Text size for menus, headers and lists: "Normal" (default) / "Large". The reader's own status bar is not affected.
- **Home screen** (submenu): Which entries sit on the Home screen; the rest move behind **More**. See **[Home Screen](#31-home-screen)**.
- **Show Busy Indicator**: Shows a small hourglass in the middle of the screen when a button press starts a screen change that takes a second or more, such as opening or leaving a book, so you can tell the press was received. It only appears on devices whose screen refreshes fast enough for it to help, so never on the LilyGo T5 S3. "ON" (default) / "OFF".

#### 3.7.2 Reader

- **Reading Orientation**: Screen orientation for reading:
  - "Portrait" (default), "Landscape CW", "Inverted", "Landscape CCW"

**EPUB Font** (submenu):
- **Font Family**: Font used for EPUB reading. Includes built-in fonts (Bookerly, Noto Sans) plus any fonts installed on the SD card.
- **Font Size**: For Bookerly and Noto Sans, 10pt to 26pt: 10pt to 20pt are typefaces designed at that size; 22, 24 and 26pt are the 20pt face enlarged. For a font on the SD card, the sizes its files were built at (the number in each file name: `MyFont_8.cpfont` is 8pt), then the standard sizes above its largest file, drawn from that file enlarged. A size the selected font does not offer, such as 8pt after switching from an SD font to Bookerly, is shown and drawn as the nearest size it does offer, and comes back when you switch back.
- **Text Anti-Aliasing**: Smooth grey edges on text. Slows page turns slightly. "ON" / "OFF"
- **Fast AA** *(X3 only)*: Swaps the slow 53-frame grayscale waveform for a fast 7-frame LUT (~130 ms). Mid-tones appear slightly darker. "ON" / "OFF"
- **Text Darkness**: Ink density for rendered text: "Normal" (default), "Dark", "Extra Dark", "Max Dark"

**TXT/Markdown Font** (submenu):
- **Font Family**: Font used when reading `.txt` and `.md` files (independent of the EPUB font).
- **Font Size**: As for EPUB: the sizes the selected font offers.

**Layout** (submenu):
- **Paragraph Alignment**: "Justified" (default), "Left", "Center", "Right", "Book Style"

**Spacing** (submenu):
- **Screen Margin**: Left/right margin in Reading Mode, 5–40 px in 5 px steps.
- **Line Spacing**: "Tight", "Normal" (default), "Wide"
- **Extra Paragraph Spacing**: "ON" adds vertical space between paragraphs; "OFF" uses first-line indentation instead.

**Images** (submenu):
- **Images**: "Display" (default), "Placeholder" (show a box where the image would be), "Suppress" (skip images entirely)
- **Large Image Placeholder**: Whether to substitute an explicit placeholder for images that are too large to display inline. "ON" / "OFF"

- **Embedded Style**: Use the EPUB's own HTML/CSS styling. "ON" (default) / "OFF"
- **Hyphenation**: Automatic hyphenation while reading. "ON" / "OFF"
- **Bionic Reading**: Bold the first half of each word to guide the eye. "ON" / "OFF"
- **Guide Dots**: Draw a small dot centered in the space between words to guide the eye along the line (idea borrowed from [CrossInk](https://github.com/uxjulia/CrossInk)). "ON" / "OFF"
- **Synthetic TOC Fallback**: Generate a table of contents from headings when the EPUB has an invalid or missing TOC. "ON" / "OFF"
- **Customise Status Bar**: Opens a submenu to configure every element of the reading status bar individually: upper and lower progress bars (Book / Chapter / Hidden, with thickness), status item position (Top / Bottom), chapter page count, book progress percentage, title display (Book / Chapter / Hidden), battery, and clock. The chapter page count and the Chapter progress bar cover the whole chapter as the table of contents lists it, even when the book splits that chapter into several files (many light novels start a new file at every illustration). A `~` before the total means part of it is still an estimate; it firms up as the rest of the chapter is laid out. On/off items are switches; the others step to their next value with **Confirm**. The preview under the list shows the result.

#### 3.7.3 Controls

- **Remap Front Buttons**: Reassign the physical function of each bottom-edge button.
- **Button Actions** (submenus — one per logical button: Back, Confirm, Left, Right, Up/Page Back, Down/Page Forward, Power): For each button, independently configure the **Short Press**, **Double Press**, and **Long Press** action. Available actions include: page forward/back, skip 10 pages, go home, sleep, force refresh, force fast refresh, open TOC, open bookmarks, star page, footnotes, next/previous chapter, exit reader, open reader menu, toggle bionic reading, KOReader sync, cycle font size, larger/smaller text, cycle orientation (either direction), quick overrides, toggle touch navigation, ignore, and — on devices with a light — toggle reading light, light brighter and light dimmer — plus light warmer and light cooler on a device with a warm/cool light.
- **Button Actions Overview**: A read-only overview screen showing the current short/double/long press mapping for every button at a glance.
- **Touch Navigation** *(touch devices only)*: Whether touch does anything outside the reader — list rows, covers, the home menu, the settings tabs, the on-screen button hints. "ON" / "OFF". See **[Touch Controls](#5-touch-controls)**.
- **Touch Page Turn** *(touch devices only)*: How the reading page turns pages by touch — "Off", "Tap Zones", "Swipe", "Tap Zones (Inverted)". It governs page turns only; the reader menu and the reading light stay reachable either way.
- **Tap Action** *(touch devices only)*: Whether tapping a list row selects it — "Select, then activate" (default) — or opens it straight away, "Activate immediately".
- **Gesture actions** *(touch devices only)*: One entry per gesture — the sideways swipes, the four edge swipes (left and right edge columns, up and down), the two ten-page zone swipes, the five tap zones, the same five as long taps, the four corner holds, pinch and rotation — each assigned an action from the same list the buttons offer. See **[Touch Controls](#5-touch-controls)**.
- **Gesture overview** *(touch devices only)*: A read-only screen drawing the tap, hold and swipe zones with your current assignments in them. Generated from live settings, so it is the authority if the documentation disagrees.
- **Tilt Page Turn** *(X3 only)*: Use the tilt sensor to turn pages by tilting the device. Sub-settings:
  - **Enable Tilt Page Turn**: "ON" / "OFF"
  - **Tilt Right action**: "None", "Next Page", "Prev Page"
  - **Tilt Left action**: "None", "Next Page", "Prev Page"

#### 3.7.4 System

- **Language**: Set the system language (see **[Supported Languages](#supported-languages)**).
- **Keyboard Layouts**: Which layouts the on-screen keyboard offers, each named in its own language: English (QWERTY), Français (AZERTY), Deutsch (QWERTZ, with ä, ö, ü and ß), Español (with ñ), and ЙЦУКЕН for Русский, Українська, Беларуская and Қазақша. Press **Confirm** on a row to switch it on or off. Until you change anything here, the keyboard offers your system language's layout plus English.
  - Once two or more layouts are on, the keyboard shows a globe key that switches to the next one.
  - One Latin layout always stays on, because web addresses and passwords need one; its row then reads "Default" and cannot be switched off.
  - On the keyboard, hold **Confirm** on a key (or long-press it on a touch screen) for its alternate letter, such as an accented one or Ukrainian ґ and the extra Kazakh letters, or for the other case. Holding **Confirm** on Delete clears the whole text. Shift applies to the next letter only, and `-`, `=`, `.` and `,` are on the symbols page (**?123**).
- **File Visibility** (submenu):
  - **Show Hidden Files**: Show files and folders whose names start with `.`, in the Library's Books tab, All Files and the web interface's file manager. The Library never lists the reader's own `.crosspoint` folder or the card's system folders. "ON" / "OFF"
  - **Show File Extensions**: Show file extensions in the Library and All Files. "ON" / "OFF"
  - **Book view**: How the Library's [Books](#33-library-screen) tab shows books. "Filenames" / "Details" / "Covers"

**Network**:
- **WiFi Networks**: Add, remove, and connect to WiFi networks. If WiFi was switched on here, leaving the screen restarts the device quickly and returns to Settings. That frees the memory WiFi used; every screen that uses WiFi does the same.
  - Whenever the reader needs WiFi, it first tries the network it last connected to, then any other saved network in range, strongest first. While it does, **Back** cancels and **Confirm** stops it and shows the network list.
  - To join a network that does not broadcast its name, choose **Add hidden network...** at the end of the list and type its name (SSID). A password saved for that name is reused; otherwise you are asked for one (leave it empty for an open network). While it connects, **Back** abandons the attempt and returns to the list.
- **KOReader Sync**: Configure and authenticate KOReader progress sync. See [KOReader Sync Quick Setup](#377-koreader-sync-quick-setup).
- **OPDS Servers**: Manage OPDS libraries. See [OPDS Servers (Multiple Libraries)](#375-opds-servers-multiple-libraries).

**Tools**:
- **Clock Settings** (submenu):
  - **Use Clock**: Enable the software clock. "ON" / "OFF"
  - **Clock Format**: "24h" / "12h"
  - **Timezone**: Pick your timezone from a full-screen list of 86: named places and regions that follow their own daylight-saving rules, fixed UTC offsets for anywhere the list misses, and plain UTC.
  - **Detect Timezone**: Auto-detect timezone via IP geolocation (requires WiFi).
  - **Sync Time**: Sync the clock via NTP (requires WiFi). Afterwards the device restarts quickly and returns to Clock Settings, like every other screen that uses WiFi.
- **Weather Settings**: Configure the Open-Meteo weather panel shown on the Home Screen. **Location** searches for a city: pick one of the matches with **Confirm**, or press **Back** to keep the current location. A search with no matches says so instead of returning silently.
- **All Files**: Browse every file on the SD card, not just books: images, cover and `.opf` files, firmware images and anything else. It works like the Library's [Books](#33-library-screen) tab: **Confirm** opens a book or image, and on any other file it opens the menu, where you can **Move to folder** or **Remove** it. Images offer **Set Sleep**, which makes the image your sleep screen. Moving or removing a book here moves or removes that file only, not the cover and `.opf` beside it. **Back** at the top folder returns to Settings.

**System**:
- **Clear Reading Cache**: Clear the internal SD card cache.
- **Repair Screen**: Clears ghosting left behind by fast page refreshes, by driving every pixel hard between black and white several times. Takes about 20 seconds and deletes nothing. A maintenance action, not a fix for ghosting while you read.
- **System Information**: Display device info (firmware version, hardware, memory, SD card). When it runs to more than one page, the header shows the page ("1 / 2") and the page buttons move between pages, wrapping round at the ends. How many pages there are depends on the orientation.
- **Boot Diagnostics**: How this boot started, where the last sleep stopped, and the history pairing each sleep with the boot that followed it. One screenful, meant to be photographed into a bug report when the device fails to sleep or fails to wake.
- **Reading Statistics**: View reading stats (streaks, time read, pages/min, per-book ETA, sparkline history). Also on the Home screen as **Reading Stats**. To forget one book, open it under **All books** and press **Confirm** (**Remove**); its reading time comes out of the totals and the book file itself is untouched. Not offered for the book you currently have open. The web dashboard's **Stats** page has the same **Remove from stats** button in each book's expanded row.

**Firmware Update**:
- **Check for Updates**: Check for and download Witch Reader firmware updates over WiFi. When a newer release is found, the question to update shows the release's title and the first few lines of its release notes, so you can see what it brings before you install it. The download is always checked against the certificate of GitHub's servers (**Skip HTTPS validation** does not apply), and the firmware against the SHA-256 checksum the release lists, before the reader switches to it. The reader restarts when you leave this screen.
- **Include Beta Updates**: Whether to include release-candidate builds in update checks. "ON" / "OFF"
- **SD Firmware Update**: Flash a firmware `.bin` file from the SD card. Press **Confirm** on a file to flash it. The **Options** button hint in the file picker opens a smaller menu than the Library's: the sort and visibility options, **Search**, **Search all folders**, and **Remove** to delete a `.bin` you no longer need.

#### 3.7.5 OPDS Servers (Multiple Libraries)

Witch Reader supports saving multiple OPDS servers and switching between them when browsing catalogs.

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
For web-based WiFi network management, see [Web Settings (WiFi + OPDS)](#376-web-settings-wifi--opds).

**Browsing a catalog.** Open it from **File Transfer → OPDS Browser**. The catalog follows **[Moving through lists](#moving-through-lists)**.
A short **Left** opens **Search** when the catalog offers one, and a short **Right** opens **Info**
for the selected book. Where either does not apply, that button does nothing on a short press,
and its hint shows only the arrow. Hold **Left** / **Right**, or swipe on a touch screen, to page
through a long catalog; **Up** / **Down** move one row. **Confirm** opens a folder or downloads a
book. When a book comes in several formats, you choose one from a short list that works the same
way.

#### 3.7.6 Web Settings (WiFi + OPDS)

While in **File Transfer** mode, the web settings page has **Wi-Fi networks** and **OPDS servers** in its menu, to manage both.

1. On device: open **File Transfer** and connect to WiFi.
1. In a browser, open `http://<device-ip>/settings` or `http://crosspoint.local`.
1. In **Wi-Fi networks**, add, edit, or delete saved network entries (SSID + optional password).
1. In **OPDS servers**, add, edit, or delete OPDS catalogs.

Behavior notes:

- Passwords are never shown back in the web UI after saving.
- Leaving Password blank while editing keeps the existing saved password unchanged.
- The web UI can save hidden-network SSIDs. On the device, join one through **Add hidden network...** in the network list; its saved password is reused.

#### 3.7.7 KOReader Sync Quick Setup

Witch Reader can sync reading progress with KOReader-compatible sync servers automatically and bidirectionally. It also interoperates with KOReader apps/devices when they use the same server and credentials.

##### Option A: Free Public Server (`sync.koreader.rocks`)

1. Go to **Settings → System → KOReader Sync**.
2. Set **Sync Server URL** to `https://sync.koreader.rocks` (or leave it empty — the default points to the same server).
3. Enter your **Username** and **Password**.
4. Select **Register** to create a new account on the server — Witch Reader handles the registration on-device, including the required MD5 password hashing. If the username is already taken, choose a different one and try again.
5. Once registration succeeds, select **Authenticate** to confirm the credentials are working.

Already have KOReader Sync credentials? Skip **Register** and go straight to **Authenticate**.

##### Option B: Self-Hosted Server (Docker Compose)

1. Start a sync server on your computer or home server:

```bash
mkdir -p kosync-quickstart && cd kosync-quickstart

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

docker compose up -d
```

> [!NOTE]
> Set `ENABLE_USER_REGISTRATION=false` after creating your accounts to prevent unexpected registrations.

2. On the device, go to **Settings → System → KOReader Sync**:
   - Set **Sync Server URL** to `http://<server-ip>:17200` (or `https://<server-ip>:7200` for the TLS listener).
   - Enter your **Username** and **Password**.
   - Select **Register** to create the account directly from the device.
   - Select **Authenticate** to confirm.

##### Using sync while reading

Press **Confirm** to open the reader menu and go to its **Sync** tab:
- **Pull progress from other devices** — jump to the position stored on the server.
- **Push progress from this device** — send the current position to the server.
- **Compare progress with other devices** — show which side is further on and let you choose. A long **Confirm** while reading does the same.

A synced position lands on the exact page, in both directions, when the page starts in text, and which side is further is decided by chapter and paragraph rather than by percentage. To fetch your progress before a book opens, hold **Confirm** on it in the Library, or choose **Fetch Progress & Open** in its options menu.

##### Automatic sync

**On Closing a Book** sends your position to the server when you close a book: "Always", only after a session of at least a set number of pages, or "Never".

On the **X4 Pro** and the **LilyGo T5 S3**, sync can also run in the background. That is useful if you read on more than one device and rarely close a book. These options sit in the **Automatic Sync** submenu of the KOReader Sync settings, and all of them are off by default:

- **Receive → On Wake**: Fetch the position from the server when the device wakes.
- **Send → While Reading**: Send your position every set number of pages, or "Never".
- **Send → On Sleep**: Send your position when the device goes to sleep.
- **Send → On Closing a Book**: As above.
- **Status Bar → Show Sync Indicator**: Show a small indicator in the reader's status bar while a background sync is running.

A background sync leaves WiFi on if the screen you are using needs it. The X3 and X4 do not have enough memory to sync in the background, so they offer **On Closing a Book** only.

##### Sending the book's details

**Send Document Metadata** (off by default) adds the book's details to every position it sends: its file name, title and authors, and, when the book gives them, its ISBN, Amazon ASIN, series and number in the series. A self-hosted server can use them to recognise the book; KOReader's own server ignores them. The ISBN, ASIN and series come from the Library's record of the book, so a book that has never been shown in the Library sends only its file name, title and authors. A book's `.opf` metadata file overrides them as it does its title.

### 3.8 Sleep Screen

The **Sleep Screen** setting controls what is displayed when the device goes to sleep:

| Mode | Behavior |
|------|----------|
| **Dark** (default) | The Witch Reader logo on a dark background. |
| **Light** | The Witch Reader logo on a white background. |
| **Custom** | A custom image from the SD card (see below). Falls back to **Dark** if no custom image is found. |
| **Cover** | The cover of the currently open book. Falls back to **Dark** if no book is open. |
| **Cover + Custom** | The cover of the currently open book. Falls back to **Custom** behavior if no book is open. |
| **Page Overlay** | A transparent PNG composited over the current reader page — book content shows through the alpha channel. |
| **Quick Resume** | A minimal screen; waking the device returns to reading immediately. |
| **None** | A blank screen. |

The **Sleep Image Pick Mode** setting controls whether custom images are chosen **randomly** or **sequentially**.

When the device goes to sleep from a book, **Sleep Screen Info Overlay** can add a band along the bottom of the sleep image with the book's title, author and progress, in white, grey or black.

#### Cover settings

When using **Cover** or **Cover + Custom**, two additional settings apply:

- **Sleep Screen Cover Mode**: **Fit** (scale to fit, white borders) or **Crop** (scale and crop to fill the screen).
- **Sleep Screen Cover Filter**: **None** (grayscale), **Contrast** (black & white), **Inverted** (inverted black & white), **Adaptive** (stretches the picture between its own darkest and lightest points), or **Equalize** (spreads the tones by how much of the picture carries them).

#### Custom images

To use custom sleep images, set the sleep screen mode to **Custom** or **Cover + Custom**, then place images on the SD card:

- **Multiple Images (recommended):** Create a `.sleep` directory in the root of the SD card and place any number of `.bmp` or `.png` images inside. (A directory named `sleep` is also accepted as a fallback.)
- **Single Image:** Place a file named `sleep.bmp` or `sleep.png` in the root directory. While it is there, it is shown instead of the images in the `.sleep`/`sleep` directory.

If the picked image cannot be shown, for example a damaged PNG, the next one in the folder is tried, up to three in all, before the default sleep screen is shown.

> [!TIP]
> For best results:
> - Use PNG (with alpha channel for Page Overlay mode) or uncompressed BMP files with 24-bit color depth.
> - Use a resolution of 480×800 pixels to match the device's screen resolution.

---

## 4. Reading Mode

Once you have opened a book, the button layout changes to facilitate reading.

### Page Turning
| Action            | Buttons                              |
| ----------------- | ------------------------------------ |
| **Previous Page** | Press **Left** _or_ **Volume Up**    |
| **Next Page**     | Press **Right** _or_ **Volume Down** |

The role of the volume (side) buttons can be swapped in the **[Controls Settings](#373-controls)**.

If the **Short Power Button Click** setting is set to "Page Turn", you can also turn to the next page by briefly pressing the Power button.

### Chapter Navigation
* **Next Chapter:** Press and **hold** the **Right** (or **Volume Down**) button briefly, then release.
* **Previous Chapter:** Press and **hold** the **Left** (or **Volume Up**) button briefly, then release.

This feature can be disabled in the **[Controls Settings](#373-controls)** to help avoid changing chapters by mistake.


### System Navigation
* **Leave the book:** Press **Back** to close the book and return to where you opened it: the **[Home](#31-home-screen)** screen, or the **[Library](#33-library-screen)** at the tab, the folder or author, and the row you opened it from. Holding **Back** does the same, unless you give the long press an action of its own under **[Controls](#373-controls)**.
* **Back after a link:** After you follow a link in the book, such as a footnote, **Back** first returns to the page you followed it from, step by step for the last three links. These steps are kept when the device sleeps or you close the book there.
* **Reader Menu:** Press **Confirm** to open the reader menu, which includes: **[Table of Contents](#6-chapter-selection-screen)**, bookmarks, sync progress, reading statistics, quick per-book overrides (font, images, hyphenation, bionic reading…), take screenshot, and more. Its entries are grouped into tabs. While the tab bar is selected, **Confirm** moves to the next tab, and its button hint names that tab. **Hold Up / Down** to switch tabs: each opens where you left it. **Hold Back** to close the book as **Go Home** does; changes made in the menu are kept.
* **Your place is kept by paragraph as well as by page.** If a book is laid out differently the next time you open it, for example after you changed the font size from outside the book or after a firmware update re-indexed it, it opens at the paragraph you were reading rather than at a page number scaled to the new length.

### Supported Languages

Witch Reader renders text using the following Unicode character blocks, enabling support for a wide range of languages:

*   **Latin Script (Basic, Supplement, Extended-A):** Covers English, German, French, Spanish, Portuguese, Italian, Dutch, Swedish, Norwegian, Danish, Finnish, Polish, Czech, Hungarian, Romanian, Slovak, Slovenian, Turkish, and others.
*   **Cyrillic Script (Standard and Extended):** Covers Russian, Ukrainian, Belarusian, Bulgarian, Serbian, Macedonian, Kazakh, Kyrgyz, Mongolian, and others.

What is not supported: Chinese, Japanese, Korean, Vietnamese, Hebrew, Arabic, Greek and Farsi.

---

## 5. Touch Controls

*Applies to the touch devices only — the **LilyGo T5 S3 Pro** and the **Xteink X4 Pro**.
The X3 and X4 have no digitiser, nothing in this section appears in their Settings, and
the code behind it is not even built into their firmware.*

Touch is an addition to the buttons, never a replacement: every button still does exactly
what it did, and everything described here can be switched off.

Two things are worth knowing before the tables:

- **What each zone does is a setting; where the zones are is not.** Every gesture below
  can be reassigned under **Settings → Controls → Gesture actions**, using the same list
  of actions a physical button offers.
- **The device can show you its own answer.** **Settings → Controls → Gesture overview**
  draws the zones with *your* current assignments in them. It is generated from your live
  settings, so it is right even when this page is out of date — trust it over this text.

### 5.1 The two master switches

Both live under **[Settings → Controls](#373-controls)**:

| Setting | Governs | Default |
| --- | --- | --- |
| **Touch Navigation** | Everything **outside** a book: list rows, book covers, the home menu, the settings tabs, the on-screen button hints, the edge gestures | On |
| **Touch Page Turn** | The **reading page** itself: how touch turns pages | Tap Zones |

They are separate on purpose. Turning off page-turn taps so a resting thumb cannot flip a
page should not also stop you tapping a book in the library, and vice versa.

Neither can strand you:

- With **Touch Page Turn** off, the reader menu is still reachable — by the swipe up from
  the bottom edge, by the centre tap, and by the Confirm button.
- With **Touch Navigation** off, the menus are button-only. On these devices Back and
  Confirm come from the capacitive Home key, and the firmware feeds those in as *button*
  presses, below the level this setting acts on.

### 5.2 Menus, lists and everything outside a book

Outside a book, **taps belong to whatever is on screen** — a row, a cover, a keyboard key,
a button hint. Only swipes and corner holds are treated as gestures there.

**Lists use point-then-confirm.** The first tap on a row *moves the highlight* to it and
stops. A second tap on the row that is already highlighted *opens* it. A mis-tap therefore
costs one more tap instead of an action to undo, and on e-paper the highlight moving is
the only feedback there is. If you would rather a row opened on the first tap, set
**Settings → Controls → Tap Action** to *Activate immediately*.

**Tabs are a single tap.** Tapping Display / Reader / Controls / System in Settings,
Navigation / Settings / Sync / Tools in the reader menu, or Books / Recent / New / Authors in
the Library, switches category immediately —
you can already see which tab you are choosing, so there is nothing to confirm.

**The button hints are tappable.** The labelled boxes along the bottom edge, and the side
hints, do exactly what the physical button under each label does — including any remapping
you have configured. *Holding* a hint is the same as holding that button.

**Exit buttons.** File Transfer, its hotspot, Calibre Wireless and USB Drive (while it waits for
a computer) also show an **Exit** button below their status on a touch screen. It does what
**Back** does.

| Gesture | What it does |
| --- | --- |
| Tap a row, cover, folder or button hint | Select it; tap again to open it |
| Hold a button hint | The same as holding that button |
| Swipe up / down over a list | Page the list (on Bookmarks, Starred pages and Wi-Fi networks, a swipe does nothing for now) |
| Swipe **right from the left edge** | Back |
| Swipe **down from the top edge** | Reading light panel |
| **Hold the top-left corner** | Toggle the reading light — the same as in a book |

> The LilyGo T5 S3 has a **Down** key but no **Up** key, so paging a list *backward* has no
> physical button there — a swipe is how you do it, or, on the lists that follow
> [Moving through lists](#moving-through-lists), holding the **«** hint box. Swipes work on the
> X4 Pro as well, which has both keys.

### 5.3 The reading page: taps and holds

The page is divided into three columns. The outer columns turn pages over their whole
height; the middle column is split into three.

```
 +-----+---------------------+-----+
 |     |        Top          |     |
 |     +---------------------+     |
 | Prev|       Centre        | Next|
 | page|                     | page|
 |     +---------------------+     |
 |     |       Bottom        |     |
 +-----+---------------------+-----+
```

| Gesture | Default |
| --- | --- |
| Tap left column | Previous page |
| Tap right column | Next page |
| Tap centre | Reader menu |
| Tap top / bottom | *(nothing — reserved for vertical gestures)* |
| Hold left / right column | Previous / Next chapter |
| Hold centre | Look the word up in the dictionary |
| Hold bottom | Star this page |
| Hold top | *(nothing)* |

**Touch Page Turn** decides what the two outer columns do:

| Mode | What the page does |
| --- | --- |
| **Tap Zones** *(default)* | Tap the left third for the previous page, the right third for the next |
| **Tap Zones (Inverted)** | The same, mirrored |
| **Swipe** | Horizontal swipes turn pages; the taps stay free |
| **Off** | The page ignores touch for page turns |

The centre tap for the reader menu has no switch of its own. To turn it off, bind
**Tap centre** to *Ignore (do nothing)* in *Gesture actions* — the bottom-edge swipe and
the Confirm button still reach the menu.

### 5.4 The reading page: swipes

**A vertical swipe is decided by where your finger starts, not where it ends.** Put your
finger on the control you want, then move; you never have to judge distance. A swipe needs
about 60 px of travel to count as a swipe rather than a tap.

```
 +---------------------------------+
 |    top edge: reading light      |
 +-----+---------------------+-----+
 |     |                     |     |
 |Bright|   < 10 pages >     |Warmth|
 | ness |                    |      |
 |     |                     |     |
 +-----+---------------------+-----+
 |   bottom edge: reader menu      |
 +---------------------------------+
```

| Gesture | Default |
| --- | --- |
| Swipe **down** from the top edge | Reading light panel |
| Swipe **up** from the bottom edge | Reader menu |
| Swipe up / down in the **left edge** column | Light brighter / dimmer |
| Swipe up / down in the **right edge** column | Light warmer / cooler |
| Swipe **left** inside the left (back) column | Skip 10 pages back |
| Swipe **right** inside the right (forward) column | Skip 10 pages forward |
| Swipe left / right elsewhere | Next / Previous page *(in Swipe mode only)* |

The left and right edge columns stop short of the top and bottom bands, so a swipe starting
in a corner cannot mean two things at once. A vertical swipe down the **middle** of the page
does nothing on purpose — it stops a thumb resting mid-page from dimming the screen.

The ten-page skips read as one rule with the taps beside them: the left column already means
*back*, so a tap there goes back one page and a flick the same way goes back ten.

### 5.5 Corners, and two fingers

**The four corners answer only to a hold, never a tap**, so adding them changed the meaning
of no tap anywhere. Each corner is one eighth of the screen's *shorter* side, which keeps it
square and the same physical size whichever way you hold the device.

| Gesture | Default |
| --- | --- |
| **Hold the top-left corner** | **Toggle the reading light** |
| Hold the other three corners | *(nothing — free for you to assign)* |

The top-left corner hold works on **every screen**, not just while reading. One gesture, one
meaning, wherever you are — a reader should not have to know which screen they are on to turn
the light off.

> The reading light is the one control the brightness swipes cannot reach: swiping down only
> dims to a minimum, and swiping up on a dark screen turns the light *on*. Turning it **off**
> needs its own gesture, and a corner hold is deliberate enough not to fire while you shift
> your grip.

**Double-press the Power button** for the same thing without touching the screen at all. A
single press still puts the device to sleep, so nothing about the button's usual job changes.
This is the route to reach for in the dark: no overlay can cover a physical button.

Two-finger gestures work on the devices whose touch controller reports more than one finger,
which is both of them:

| Gesture | Default |
| --- | --- |
| Pinch in / out | Smaller / Larger text |
| Rotate clockwise / anticlockwise | Change orientation forward / back |

Turning two fingers back the way they came undoes the rotation rather than advancing three
more steps.

### 5.6 Reading light

*Devices with a frontlight or backlight only.*

Under **Settings → Display → Reading light**:

- **Reading Light** — on / off
- **Brightness** — 1–100%. The scale is perceptual, so the low end has as many usable steps
  as the top; there is deliberately no 0%, because that is what the on/off switch is for
- **Warmth** — 0 (fully cool) to 100 (fully warm). Only on a device with a second warm
  channel. Brightness is the *total* light and warmth splits it between the two LED strings,
  so changing it shifts the colour without changing how bright the page looks
- **Restore Light on Wake** — whether waking brings the light back. Brightness and warmth are
  always remembered; this only controls the on/off state

Both sliders **preview as you move them**, so you can judge the level by looking at the page
rather than at a number. Cancel puts back what you had. If the light was off when you opened
the slider, previewing lights it and both Cancel *and* Confirm put it back off — asking for a
brightness is not asking for the light to come on.

The quickest routes to it without opening Settings:

- **Swipe down from the top edge** for the light panel, on any screen
- **Hold the top-left corner**, or **double-press Power**, to toggle it
- **Swipe up / down at the left edge** to brighten and dim, and at the right edge for warmth

Adjusting brightness while the light is off turns it on — a brightness gesture always means
"I want light". These all work on the home screen and in menus as well as in the reader, so
you can find a book in the dark.

The same actions are bindable to any button or gesture: **Toggle Reading Light**, **Light
Brighter**, **Light Dimmer**, and on a warm/cool device **Light Warmer** and **Light Cooler**.

### 5.7 Per-device differences

| | X4 Pro | LilyGo T5 S3 |
| --- | --- | --- |
| Physical keys | Up, Down, Power | Down, Power |
| Back / Confirm | Home key: hold / tap | Home key: hold / tap |
| Top edge, swipe down | Reading light | Reading light |
| Bottom edge, swipe up | Reader menu | Reader menu |
| Right edge, swipe up/down | Light warmer / cooler | *(free — single-channel light)* |

On a touch device **without** a reading light the two vertical edges swap roles: the top edge
becomes the reader menu, there being no light panel to put there, and the bottom edge returns
Home.

### 5.8 Turning it off

- **Settings → Controls → Touch Page Turn** → *Off* stops touch turning pages, and nothing
  else.
- **Settings → Controls → Touch Navigation** → *Off* silences touch outside the reader.
- **Settings → Controls → Tap Action** switches list rows between select-then-open and
  open-on-first-tap.
- Any single gesture can be set to **Ignore (do nothing)** in *Gesture actions* to switch just
  that one off. This is different from **Built-in**, which means "leave this to the screen" —
  each row names what Built-in will actually do, so "Swipe left" reads *Next Page* while you
  are in Swipe mode and *Nothing* when you are not.
- The action **Toggle Touch Navigation** can be bound to a button, if you want to silence
  touch without going into Settings.

### 5.9 If a gesture does nothing

1. Open **Settings → Controls → Gesture overview** and check what that zone is actually
   assigned to. A gesture showing *Built-in* does whatever the reader would have done anyway,
   which for most zones is nothing.
2. A vertical swipe must **start** in the edge column or band it belongs to. Starting
   mid-page is unassigned by design.
3. A swipe needs about 60 px of travel to register as a swipe rather than a tap.
4. Light and warmth actions do not appear on a device without that hardware, and two-finger
   actions do not appear on a controller that reports a single finger. The overview page only
   ever shows what your device can do.
5. Corner zones answer to a **hold**, not a tap.

For the developer-facing account of how this was built and why each default was chosen, see
[`docs/touch-gestures.md`](docs/touch-gestures.md) and
[`docs/contributing/touch-architecture.md`](docs/contributing/touch-architecture.md).

---

## 6. Chapter Selection Screen

Accessible by pressing **Confirm** while inside a book and selecting **Table of Contents**.

1.  Use **Left** (or **Volume Up**), or **Right** (or **Volume Down**) to highlight the desired chapter.
2.  Press **Confirm** to jump to that chapter.
3.  *Alternatively, press **Back** to cancel and return to your current page.*

---

## 7. Current Limitations & Roadmap

Please note that this firmware is currently in active development. The following features have known limitations:

* **Cover Images:** Large cover images embedded into EPUB require several seconds (~10s for ~2000 pixel tall image) to convert for the sleep screen and home screen thumbnail. Uploading the book with **Optimize EPUB** in the web interface's File Manager, or optimizing it there afterwards, resizes them and speeds this up (see [Optimizing EPUBs](./docs/webserver.md#optimizing-epubs)).
* **Right-to-left scripts (Hebrew, Arabic):** Not currently supported. For BiDi / RTL support, use the original [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) firmware.
* **CJK (Chinese, Japanese, Korean):** Not supported. See https://github.com/aBER0724/crosspoint-reader-cjk for a CJK-capable fork.

---

## 8. Troubleshooting Issues & Escaping Bootloop

If an issue or crash is encountered while using Witch Reader, feel free to raise an issue ticket and attach the serial monitor logs. The logs can be obtained by connecting the device to a computer and starting a serial monitor. Either [Serial Monitor](https://www.serialmonitor.org/) or the following command can be used:

```
pio device monitor
```

If the device is stuck in a bootloop, press and release the Reset button. Then, press and hold on to the configured Back button and the Power Button to boot to the Home Screen.

There can be issues with broken cache or config. In this case, delete the `.crosspoint` directory on your SD card (or consider deleting only `settings.json`, `state.json`, or `epub_*` cache directories in the `.crosspoint/` folder).

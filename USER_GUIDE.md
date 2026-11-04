# CrossPoint User Guide

Welcome to CrossPoint. Start with your first book below, then explore the reading tools and settings at your own pace.

This guide covers everyday use on button and touch devices. Some controls depend on your device. If you still need to install CrossPoint, see [Install firmware](README.md#install-firmware).

- [Start reading](#start-reading)
- [Find and manage books](#find-and-manage-books)
- [While reading](#while-reading)
- [Make reading comfortable](#make-reading-comfortable)
- [Transfer and sync](#transfer-and-sync)
- [Settings reference](#settings-reference)
- [Help and recovery](#help-and-recovery)

## Start reading

### Add your first book

CrossPoint reads EPUB, XTC/XTCH, and TXT books. It also opens BMP images.

To copy a book from your computer:

1. Put the reader to sleep and remove its SD card.
2. Insert the card into your computer and copy a book to it. You can use folders to organize your books.
3. Eject the card, put it back in the reader, and turn the reader on.

You can also send a book from your phone or computer over Wi-Fi:

1. On the reader, open Home → File Transfer → Join a Network and connect to your Wi-Fi network.
2. On a phone or computer connected to the same network, open the web address shown on the reader.
3. Open the file manager in your browser and upload a book.
4. Leave File Transfer on the reader and open Browse Files.

Use a trusted private network. The file transfer web interface does not require a password.

For a direct connection without a router, see [Wi-Fi file transfer](#wi-fi-file-transfer).

### Open a book

On first launch, CrossPoint opens Home. Select Browse Files, open the folder that contains your book, then select the book.

| Action | Buttons | Touch |
| --- | --- | --- |
| Move through a list | Left/Right or Side Up/Side Down | Swipe to scroll |
| Open a book or folder | Confirm | Tap its row |
| Go back in menus | Back | Tap the back arrow or swipe right from the left edge |
| Open the reader menu | Confirm while reading | Tap the center of the page |
| Turn to the next page | Right or Side Down | Swipe left |
| Turn to the previous page | Left or Side Up | Swipe right |
| Return Home | Back while reading | Open the reader menu and choose Go Home |

These are the default controls. For right-to-left EPUBs, the page-turn swipe directions are reversed. You can change gestures and button actions in Settings → Controls.

On the Xteink X4, the bottom buttons are Back, Confirm, Left, and Right. The right side has Power, Side Up, Side Down, and Reset. Other devices have different layouts.

To return to Browse Files from a book, hold Back. The Short Back to File Browser setting can swap the short and long Back actions.

### Sleep and resume

Hold Power for about half a second to sleep or wake the reader. CrossPoint reopens the last book on subsequent starts.

The reader also sleeps after a period of inactivity. Change this in Settings → Display → Time to Sleep. To use a short Power press for sleep, change Short Power Button Click in Settings → Controls.

## Find and manage books

### Home

Home gives you access to your recent reading, Browse Files, Library, File Transfer, and Settings. The Plugins entry appears when plugins are enabled.

### Library

Library collects up to 4,096 supported books from the SD card. It shows titles and authors, so you do not need to remember folder names.

| Tab | What it shows |
| --- | --- |
| Recent | Recent reading first, followed by the rest of your books in file-date order |
| Title | Books grouped by the first letter of their title |
| Author | Books grouped by author |

An arrow beside a tab shows its sort direction. Title and Author sort A–Z or Z–A. Recent can also show the oldest files first. Titles that start with numbers or punctuation appear under `#`. Non-English letters have their own groups.

With buttons, use the direction buttons to select a book, then press Confirm to open it. Hold a direction button to move a page at a time.

Press Back to focus the tabs. Press Confirm to change tabs, or hold Confirm to reverse the sort direction. Use the button labeled Search to search, or Down to return to the list.

With touch, tap a tab or book, swipe to scroll, and tap the Search icon to search. Tap the active tab again to reverse its sort direction.

In Title or Author, hold Confirm on a book, or long-press its row, to show the letter or author groups. Select a group to expand it. With buttons, Back restores your previous book selection. In Recent, hold Confirm on a book, or long-press its row, to open its action menu.

Library creates its index the first time you open it. After adding books or changing their title or author information, choose Refresh library from a Recent book's action menu. On touch devices, you can also use the refresh icon in the Library header. The Use Book Metadata setting under Settings → Library controls whether the index reads titles and authors from inside books.

### Browse Files

Browse Files shows the folders and filenames on your SD card. The current folder path appears at the top. Hidden files and folders stay hidden unless you enable Settings → System → Show Hidden Files.

Use the direction buttons and Confirm, or tap a row, to open a folder or book. Hold a direction button to move a page at a time. Opening a BMP file shows the image viewer.

Hold Confirm on a selected item, or long-press its row, to open its action menu. Files offer Open, Delete, and Rename. Folders offer Open and Delete. Deletion asks you to confirm. To move files or manage them from a computer, use the [file transfer web interface](docs/webserver.md#file-manager).

## While reading

The tools in this section describe EPUB reading. Other book formats can offer fewer options.

### Reader menu

Press Confirm or tap the center of the page to open the reader menu. Settings → Reader → Reader Menu Style offers List and Toolbar layouts.

In List mode, select an action directly. In Toolbar mode, use Contents for chapters, Text for appearance, and More for additional actions. Use the direction buttons and Confirm, or tap the controls. Press Back to close the list menu. In Toolbar mode, Back returns from a panel to the toolbar, then closes it.

| Action | Use it to |
| --- | --- |
| Select Chapter / Contents | Open the table of contents |
| Go to % | Jump to a percentage of the book |
| Toggle Bookmark | Add or remove a bookmark on the current page |
| Bookmarks | Open saved bookmarks |
| Links and Footnotes | Choose a link on the page |
| Look Up | Look up text in your selected dictionary |
| Create Clipping / View Clippings | Save or read excerpts |
| Text Settings / Text | Change the appearance of text |
| Reading Orientation | Rotate the page |
| Auto Turn (Pages Per Minute) | Set automatic page turns |
| Take screenshot | Save the screen to `screenshots/` on the SD card |
| Show page as QR | Show a QR code for the current reading position |
| Sync Progress | Sync this book's reading position |
| Bluetooth Page Turner | Toggle Bluetooth for a paired remote |
| Go Home | Close the book and return Home |
| Delete Book Cache | Rebuild this book's cached layout on the next open |

Some actions appear only when they are available. For example, Look Up requires a selected dictionary, and Bookmarks appears after you save a bookmark.

### Chapters and page navigation

Open Select Chapter in the list menu, or Contents in the toolbar. Select a chapter with the direction buttons and Confirm, or tap it.

To enable chapter shortcuts, set Settings → Controls → Long-Press Button Behavior to Chapter Skip. Hold Right or Side Down to skip forward, or Left or Side Up to skip back. This behavior is off by default.

Auto Turn advances pages at the speed you choose in the reader menu. Tilt Page Turn is also available in Controls on devices with a supported tilt sensor.

### Bookmarks

Choose Toggle Bookmark in the reader menu to save or remove a bookmark on the current page. For a button shortcut, assign Bookmark in Settings → Controls → Long-Press Menu, then hold Confirm while reading. This shortcut is disabled by default.

Open Bookmarks in the reader menu and select a saved bookmark to return to it. Hold Confirm on a bookmark, or long-press its row, to open its action menu. Choose Rename to name it, or Delete to request deletion.

### Links and footnotes

On touch devices, tap a link in the text. With buttons, open Links and Footnotes from the reader menu and select a link. To return to where you followed it, press Back or swipe right from the left edge on a touch device.

CrossPoint keeps the three most recent return positions. If you close the book or let the reader sleep after following a link, it reopens at the page you were viewing. Back still returns to the earlier location.

### Dictionary lookup

You can look up words offline with a StarDict dictionary on the SD card:

1. Copy a supported dictionary into `/dictionaries/`.
2. Select it in Settings → Reader → Dictionary.
3. While reading, choose Look Up from the reader menu.
4. Use Left/Right to select a word, then press Confirm to read its definition.

On touch devices, hold a word and choose Look Up from the selection toolbar. You can also assign Dictionary to the Long-Press Menu shortcut.

See the [dictionary guide](docs/dictionary.md) for supported files and installation details.

### Clippings and highlights

A clipping saves an excerpt from a book and highlights the selected text.

With buttons:

1. Choose Create Clipping from the reader menu.
2. Move to the first word and press Confirm.
3. Move to the last word and press Confirm again.
4. Choose Clip to save the selection.

With touch:

1. Hold a word to start a selection.
2. Drag the selection handles to adjust the range.
3. Tap Clip above the selection to save it.

The selection tools also offer Look Up and Bookmark. To read saved excerpts, open View Clippings from the reader menu. Select an excerpt to open it or use Go to Passage where available. Hold Confirm, or long-press an excerpt, to delete it.

CrossPoint also exports saved excerpts to `My Clippings.txt` in the SD card's root folder. Deleting a clipping leaves that text export unchanged. If Sync Clippings is enabled, individual clipping deletions are sent during the next manual sync.

See [clipping selection, export, and sync details](docs/clippings.md) for longer selections, limits, and the effects of deleting a book.

### Screenshots

Choose Take screenshot from the reader menu. On devices with side buttons, you can also press Power and Side Down together. Screenshots are saved in `screenshots/` on the SD card. You can download them through File Transfer.

## Make reading comfortable

### Text appearance

Open Settings → Reader → Text Settings. While reading, you can also open Text Settings from the list menu or Text from the toolbar.

| Tab | What you can change |
| --- | --- |
| Font | Built-in Noto Sans or an installed font family |
| Size | Text size, using the sizes available for that family |
| Layout | Line, word, and character spacing, paragraph spacing and indentation, alignment, and screen margins |
| Style | Focus Reading, hyphenation, embedded book styles, and text anti-aliasing |

Built-in and direct TTF/OTF/TTC fonts offer 12, 14, 16, and 18 pt. Converted `.cpfont` families offer their installed sizes.

Embedded Style uses formatting from the EPUB. Text anti-aliasing smooths letter edges but slightly slows page turns. Focus Reading bolds the first part of each word. Hyphenation changes where words can break across lines. For Korean text, it allows breaks between syllables without drawing a hyphen.

Choose Reading Orientation to rotate the page. Settings → Reader also contains image-display choices and Customise Status Bar.

### Add fonts and read other languages

To install a font without converting files yourself:

1. Open Settings → Reader → Manage Fonts.
2. Choose a font family and download it over Wi-Fi.
3. Open Settings → Reader → Text Settings → Font and select the family.

Custom fonts add scripts that the built-in book fonts lack, including Chinese, Japanese, Korean, Arabic, Greek, Hebrew, and Farsi. The interface language and book fonts are separate choices. Changing the interface language does not install a font for your books.

Noto Serif and OpenDyslexic are optional downloads. The [SD card font guide](docs/sd-card-fonts.md) covers manual installation and supported font formats. `.cpfont` files work on all devices. Direct TTF/OTF/TTC loading requires a device on which CrossPoint enables external RAM.

### Control Center and frontlight

On touch devices, swipe down from the top edge to open Control Center. You can also tap the status bar on Home, Browse Files, or Settings.

If your device has a frontlight, drag the brightness slider or tap the sun icon to switch the light on or off. Devices with adjustable warmth also show a warmth slider. The X4 Pro offers brightness and warmth. Paper Mono offers brightness only.

The quick controls provide night mode, screen refresh, reading orientation, and a toggle for reader touch controls. Builds with Bluetooth support also offer a Bluetooth toggle for a saved remote. Enabling Bluetooth here does not pair a new remote. Filled toggles are enabled, and outlined toggles are disabled. Refresh and orientation always have outlines.

Light adjustment is in Control Center rather than Display settings. On the X4 Pro, double-clicking Power also toggles the frontlight when that shortcut is enabled. Settings → Display → Restore Light on Wake controls whether the light returns to its saved state after waking.

### Sleep screen

Choose Settings → Display → Sleep Screen to change what appears while the reader sleeps.

| Mode | What appears |
| --- | --- |
| Dark / Light | The CrossPoint logo on a dark or white background |
| Cover | The current book's cover, or Dark when no book is open |
| Custom | An image from the SD card, or Dark if no image is found |
| Cover + Custom | The cover while reading, otherwise a custom image |
| Quick Resume | The last page of text with a moon icon |
| Transparent | An overlay image on the current screen |
| Cover + Transparent | An overlay on the last book's cover, with the current screen as a fallback |
| None | A blank screen |

Quick Resume on Timeout uses the last page when the reader sleeps due to inactivity. Sleep Screen Cover Mode chooses Fit or Crop for covers and custom images. Sleep Screen Cover Filter offers grayscale, contrast, or inverted output.

See [custom sleep images](docs/sleep-images.md) for folders, image formats, transparent overlays, and fallback behavior.

### Bluetooth page turners

On builds with Bluetooth support, open Settings → Connectivity → Bluetooth Page Turner. Enable Bluetooth and choose Scan for Remotes to pair your remote. Default remote controls work without custom mapping.

To assign a button:

1. Open Map Buttons and choose an action.
2. Press the remote button within 15 seconds.
3. Release it within 15 seconds. A hold of at least 0.7 seconds is separate from a tap.

Assigning the same gesture again replaces its previous action. To clear a mapping, hold Confirm on its action or long-press the row. Back cancels learning. If the remote never reports a release, learning saves the press as a tap after 15 seconds.

Confirm and Back work like device buttons in the reader. Page-turn actions move through items while a reader menu is open. Use Paired Remotes to forget a saved remote.

## Transfer and sync

### Wi-Fi file transfer

Open Home → File Transfer. Choose a connection method:

| Method | What to do |
| --- | --- |
| Join a Network | Connect the reader and your phone or computer to the same Wi-Fi network |
| Create Hotspot | Connect your phone or computer to the reader's `CrossPoint-Reader` Wi-Fi network |
| Calibre Wireless | Connect to Wi-Fi and send books through the CrossPoint Calibre plugin |

Open the web address shown on the reader. The file manager can upload, download, rename, move, and delete files. It can also download screenshots. The web interface remains available while File Transfer is running.

The browser Settings page lets you change device settings and manage saved Wi-Fi networks and OPDS servers. Leaving a saved network or server password blank while editing keeps the existing password. Passwords are not shown after saving.

See the [web interface guide](docs/webserver.md) for connection steps, the EPUB Optimizer, WebDAV network-drive access, and command-line transfers.

### Calibre

Install the CrossPoint Reader plugin in Calibre, then set its Host to the IP address shown on your reader. On the reader, open File Transfer → Calibre Wireless. In Calibre, select books and choose Send to Device → Send to main memory.

See [Calibre wireless setup](docs/calibre-wireless.md) for installation steps and screenshots. To delete books, use Browse Files or the web interface.

### OPDS catalogs

OPDS catalogs let you browse and download books from a library server:

1. Open Settings → Connectivity → OPDS Servers.
2. Select Add Server and enter the catalog URL. A Calibre Content Server URL usually ends in `/opds`.
3. Add a display name and credentials if the server requires them.
4. Select the saved server to browse its catalog.

You can save up to eight servers. Authentication supports HTTP Basic, so configure Calibre for Basic rather than Digest authentication. To remove a server, open its settings and choose Delete Server. You can also manage servers from the web interface's Settings page.

### Reading progress sync

CrossPoint can share reading positions with other CrossPoint readers and KOReader devices that use the same server and credentials.

For the default service, open Settings → Connectivity → CrossPoint Sync. Enter a username and password, leave Sync Server URL empty, and select the CrossPoint server type. Use Sign Up on your first device and Authenticate on the others.

While reading, open the reader menu and choose Sync Progress. Smart sync resolves simple cases automatically. Ask every time lets you choose Apply Remote or Upload Local. You can also assign CrossPoint Sync to the Long-Press Menu shortcut.

Send Metadata controls whether book titles, authors, and other book details accompany progress. Sync Clippings is off by default. Enable it only with a compatible CrossPoint Sync server to include excerpts in manual syncs.

See [progress sync setup](docs/progress-sync.md) for existing KOReader accounts, server choices, sync behavior, and self-hosting.

### Plugins

To add optional reading services and tools, enable Settings → Connectivity → Plugins, then open Home → Plugins.

If Plugin Hub is offered, choose Install to download it over Wi-Fi. Choose Hide to remove the installer entry. To bring it back, set Plugins to Enabled again. You can also install plugins manually.

To uninstall a plugin, hold Confirm on it for one second, or long-press its row, then confirm. This deletes the plugin's installed folder and files. Disabling Plugins instead keeps the files on the card.

Plugins with web settings appear in the web interface while File Transfer is running. See the [plugin guide](docs/sd-plugins.md) for manual installation and further details.

## Settings reference

Open Settings from Home. The categories below match the device. Some settings appear only on devices that support them.

| Category | What you will find |
| --- | --- |
| System | Language, keyboard layouts, hidden-file visibility, clock where available, firmware updates, and About |
| Display | Sleep screen, Time to Sleep, battery percentage visibility, full-refresh frequency, theme, night mode, and light restoration |
| Reader | Text Settings, Manage Fonts, orientation, images, dictionary, reader menu style, status bar, and Clear Reading Cache |
| Controls | Button actions, page-turn gestures, touch controls, and device-specific input settings |
| Library | Use Book Metadata, Clear Read Books from Recent List, and Move Finished Books to Read Folder |
| Connectivity | Wi-Fi, Bluetooth Page Turner, CrossPoint Sync, OPDS Servers, and Plugins |

### Display choices

Refresh Frequency controls full screen refreshes while reading. More frequent refreshes clear lingering marks from previous pages, also called ghosting. Night Mode inverts the display. UI Theme offers Classic, Lyra, Lyra Extended, and RoundedRaff. Lyra Extended shows three books on Home rather than one.

Hide Battery % can hide the number in the reader or everywhere. The battery icon remains visible. Sunlight Fading Fix is available on supported button devices for screen fading in direct sunlight.

### Reader choices

Images controls pictures inside EPUBs. Display shows grayscale images with an extra grayscale refresh. Black & White uses a dot pattern without that extra refresh. Placeholder shows alternative text. Suppress leaves images out.

Customise Status Bar controls the reading information and progress display. Dictionary appears when a dictionary folder is available under `/dictionaries/`. Clear Reading Cache removes book cache folders so CrossPoint can rebuild them. Back up the SD card first if you need to preserve cached reading positions.

### Control shortcuts

| Setting | Choices or effect |
| --- | --- |
| Remap Front Buttons | Change the bottom buttons on button-only devices |
| Side Button Layout (reader) | Change the side buttons' page-turn actions or disable them |
| Long-Press Button Behavior | Off, Chapter Skip, or Orientation Change |
| Long-Press Menu | Bookmark, CrossPoint Sync, Dictionary, Create Clipping, or Disabled |
| Short Power Button Click | Ignore, Sleep, Page Turn, Links and Footnotes, or Refresh |
| Quick return from links | Use a short Power press to return after following a link |
| Touch Reader Controls | Enable or disable touch reading controls |
| Next / Previous Page Gesture | Choose taps, swipes, both, inverted taps, or Disabled |

A short Confirm press still opens the reader menu when Long-Press Menu is assigned. Power page turns and link actions apply while reading. Device-specific settings can also control a Home button, slide switch, vibration, or tilt page turns.

## Help and recovery

### The reader freezes or repeatedly restarts

On devices with a Reset button, press and release Reset, then quickly hold Power for a few seconds to reboot.

On devices with Reset and Back buttons, hold the configured Back button and Power after pressing Reset to bypass a book that repeatedly crashes. This starts at Home so you can avoid reopening the book.

If one book causes problems, try Delete Book Cache from its reader menu. If you cannot open it, back up the SD card before removing the affected `epub_*` cache folder under `.crosspoint/`.

Do not delete the whole `.crosspoint` folder as a first step. It also contains settings, bookmarks, and clippings. Back it up before changing or removing its contents. Removing `settings.json` resets preferences, and removing `state.json` resets saved startup state.

### A book looks wrong or opens slowly

Large EPUB cover images take time to convert for the sleep screen and Home thumbnails. Try the EPUB Optimizer in the web interface for books with compatibility problems.

Most JPG, PNG, and non-interlaced GIF images display in EPUBs. Animated GIFs show only their first frame. Interlaced GIFs show an outlined placeholder. If letters are missing, install a font that covers the book's language.

If the reader reports "Damaged font file", delete the affected `.cpfont` family's folder and install it again. Broken font files can leave characters blank.

### Wi-Fi transfer does not work

Make sure that your phone or computer uses the same network as the reader. In hotspot mode, connect to `CrossPoint-Reader`. Try the IP address shown on the reader if `crosspoint.local` does not work.

See [network troubleshooting](docs/troubleshooting.md) for connection drops, failed uploads, and saved-password problems.

### Update CrossPoint

Open Settings → System → Check for updates to update over Wi-Fi. You can also place `firmware.bin` on the SD card and use the SD firmware update action. The BOOX Picco uses USB updates instead. See [Install firmware](README.md#install-firmware) for USB installation instructions.

### Report a problem

Include your device model, CrossPoint version, and the steps that caused the problem in a [bug report](https://github.com/crosspoint-reader/crosspoint-reader/issues).

After a crash, look for a crash report in the SD card's root folder and attach it. For detailed serial logs, see the [debugging guide](docs/contributing/testing-debugging.md#enhanced-serial-monitor).

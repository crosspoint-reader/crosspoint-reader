# Release Notes

User-facing changes only. Full commit history is in git log.

## 2.40 — 2026-10-13

Everything since 2.37. Your books have one Library screen, with tabs for Recent, New and Authors and a view of their covers, and an SD-card font offers the sizes it was made in. KOReader sync lands on the exact page, the lists move with one set of buttons, and fonts and firmware updates download from GitHub again. The web interface has a new design and many new pages, and lives on the SD card: the first time you open it after updating, it installs itself (see the [Web Interface Guide](docs/webserver.md)). Every language is now translated in full.

### Library

- **One Library screen, with four tabs: Books, Recent, New and Authors.** Browse Files and Recent Books are now its first two tabs, and Home has a single **Library** entry, which opens on the tab you left. Hold **Up** or **Down** to change tab (their hints read "Up / Tab" and "Down / Tab"), or tap a tab on a touch screen. Each tab keeps your place while the Library is open, and closing a book returns to the tab, the folder or author, and the row you opened it from.
- **New lists the ten books added to the card most recently, and Authors everyone who wrote a book on it.** Authors are listed surname first ("Pratchett, Terry"), each with the number of their books; open one to see their books by series and then title. The author comes from an `.opf` metadata file beside the book if there is one, and its sort name decides where the author is filed; without one, the last word of the name comes first. Both tabs read an index of the card's books that is kept on the card and brought up to date when the card changes: books added or removed on the device, through the web interface, Calibre, an OPDS catalogue or USB Drive are noticed by themselves, and **Refresh library** in the options menu picks up anything copied in a computer's card reader that the Library missed. A book that has never been opened is read once for its author, so the first visit takes a while on a large card; the header shows **Indexing** while it runs. Up to 2,000 books are indexed.
- **Three ways to show books: Filenames, Details and Covers**, chosen with **Book view** in the options menu, separately for each tab. **Details** shows each book's title, with the author and series under it ("Thomas Mann · Werke #3") and how far you have read: a percentage, or **Finished**. **Covers** shows a grid of whole covers with the title and author under each, a bar along the bottom of a book you are reading and a folded corner on one you have finished. A folder is drawn as a folder, with the number of books in it and in the folders below it. In the grid, Up/Down move a row and Left/Right one cover, wrapping round at the ends; hold **Right** for the options menu. A cover not made yet shows a card with the title first; the covers on screen are then made one at a time, and a button press pauses the work. The Books tab starts on Filenames, Recent and New on Covers, and Authors on Details.
- **Covers are shown whole**, scaled to fit, in the cover grids and on the finished-book screen. The Recent Books grid and the finished-book screen used to crop them to fill the tile, which cut off a third of a typical cover.
- **The cover grid fills the T5 S3's taller screen with three rows of three**, instead of two rows of two and a fifth of the screen left blank. The X3 and X4 keep two rows of two.
- **The Books tab lists books and nothing else**: EPUB, XTC, TXT and Markdown. A cover image or `.opf` file saved beside a book is no longer listed as a file of its own, so a book downloaded from an OPDS catalogue shows up once. A folder with no book anywhere in it is left out too, and so are the reader's own `.crosspoint` folder and the card's system folders, even with **Show Hidden Files** on. **Move to folder** and **Remove** take the book's cover and `.opf` file with it, and **New Folder** opens the folder it made. Every file on the card, images included, is in the new **Settings → System → All Files**, which is also where an image's **Set Sleep** now lives.
- **Recent works like the Books tab.** It has the same three views (your old grid or list choice carries over as Covers or Details), the same buttons, and an options menu with **Remove from recents**, which takes a book off the list but not off the card, and **Go to folder**, which opens the Books tab where the book is. The old screen's long presses (Up for the view, Left to remove, Right for Info) are now in that menu.
- **An `.opf` metadata file names XTC, TXT and Markdown books too**, not just EPUBs, and Details and Covers show an XTC book's title and author from the book itself instead of its filename. The metadata editor plugin lists every book format and has a new **Sort author as** field, the name the author is filed under in Authors; it follows the author until you change it yourself.
- **Folder book counts are remembered until the device sleeps**, so going back to a folder no longer shows "..." while it counts again. Moving, removing or uploading a book counts the folders it changes again.
- **Covers draw about three and a half times faster**: a grid cover takes 12 ms instead of 40 ms on the X3, and a page of the Recent Books grid 79 ms instead of 168 ms.
- **Fix: a book could be marked as having no cover for good.** When the Home carousel or the Recent Books grid moved from one book to the next, the next book's cover was read from where the previous book's cover sat in its file, which failed, and the book was recorded as coverless. A cover that fails for want of memory, or because a button was pressed, is also tried again later instead of being given up.
- **Fix: a moved or renamed book started over.** Moving a book with **Move to folder**, or moving or renaming it, or its folder, in the web interface or over WebDAV, lost its reading position, bookmarks and place in Recent, and a rename also lost its reading stats. They now go with the book.

### Fonts

- **An SD-card font offers the sizes it was made in** (#395), such as 6, 7 or 8 pt, not only 10 to 26 pt. Above its largest size it also offers the standard sizes, drawn from its largest file enlarged; it never offers a size made by shrinking a larger file. Bookerly and Noto Sans keep 10 to 26 pt. If you switch to a font that does not offer your size, the nearest size it does offer is shown and used, and your own size comes back when you switch back. The same applies to the font size of a single book and to the TXT and Markdown font.
- **The web interface's settings page can change the font and its size in one save.** A size picked from the list is saved as picked, and the list follows a change of font.
- **The font picker makes its previews without running the X3 short of memory.** It now works in the screen's second frame buffer instead of the shared memory: the lowest free memory while it makes previews went from about 10 KB to 26 KB, and making them no longer stops halfway for lack of memory.
- Fix: the font picker made its previews again on every visit for a font drawn enlarged, such as 22 pt from an 18 pt file.
- **Fix: an SD-card font with 255 kerning classes froze the reader**, and one whose kerning tables name a class the font does not have could crash it. Such entries are now treated as unkerned.
- **The largest sizes of an SD-card font load from the flash font cache too.** Mapping a font size took the cache's whole start up to that size, so on the X3 and X4 the top one or two sizes of most catalog families did not fit and were read from the card instead, with their tables taking memory.

### Reading

- **Background pictures set by a book's stylesheet are shown.** They were never drawn: Gutenberg's illustrated *Alice* draws its rabbit hole as the background of a table, so chapter 2 showed only the text around it. Text cannot be drawn over a picture here, so the picture now follows the text of the box it belongs to. Small ornaments (under 96 pixels a side), repeating textures, list bullets and whole-page backgrounds are still left out.
- **A long paragraph keeps the space the book puts above it.** A paragraph of more than 96 words lost its top margin and padding, and with them the blank line of a scene break before it, or the space of an empty box.
- **A poem's indented lines keep their shape on a narrow screen.** In *Alice*'s "Mouse's Tale", each line of the tail is indented a step further, and on the X3 the deepest lines wrapped back to the left edge and broke the tail apart. The indent now gives way first, so a line whose words fit the column stays on one line.
- **Back after following a link survives sleep and closing the book.** Back still returns to where you followed the link (a footnote, say) after the device has slept or you closed the book there. After more than three links in a row, Back forgets the oldest instead of the newest, so it always returns to the page you just left.
- **Turning onto a page with pictures is quicker.** The reader prepares the next page while you read, but it used to skip every page with a picture. A page whose pictures have already been decoded is now prepared like a text page: on the X3, turning onto one took 677 ms and now takes 499 ms. This does not apply on the T5 S3. Drawing a page whose pictures are already decoded also no longer looks into the book first, which saved about 30 ms on the X4.
- **Fix: the screen flashed every minute on a page with pictures.** While such a page was open, each clock or battery update of the status bar was drawn with the slower full refresh instead of the quick one. The full refresh owed to a picture page now waits for the next page turn.
- Fix: after turning away from a page with a "Large image — press Confirm to load" placeholder, the first Confirm on the next page did nothing instead of opening the reader menu.
- **"Indexing" shows up less often on the X3.** The reader prepares the next chapter in the background, but on the X3 it seldom had the memory to start. It now leaves about 13 KB more free while you read (a median of 47.7 KB instead of 33.9 KB in one book), starts the work with less, and in the last three pages of a chapter carries on through page turns instead of waiting for a pause. Those few pages are drawn without anti-aliasing while it runs.
- **Very large books open on a device that has been in use.** A book of 2,956 chapters took 75 seconds and then failed, or opened without its table of contents, unless the device had just been restarted. It now opens in about 10 seconds. A table of contents lost to a memory shortage is tried again the next time the book is opened, up to three times; it used to stay lost until the book's cache was cleared.
- **Fix: a page after a very tall table row could be unreadable.** When a table cell taller than the screen was laid out as paragraphs instead, the next page could be saved damaged, and showed as an empty chapter on every visit (page 4 of chapter 2 of the illustrated *Alice*).
- **Fix: running short of memory while opening or drawing a page no longer restarts the device.** A page's footnotes, a chapter's page table, its contents entries and printed page numbers, and the font warm-up asked for memory in a way that restarts the device when it is refused. The page now does without them: its footnote links are not tappable, or the printed page number is left out.

### Touch

- **Fix: a swipe on Bookmarks, Starred pages, Wi-Fi or the OPDS catalog no longer acts on the selected row.** A vertical swipe over these lists was passed on as a Left/Right press. Swiping up on Bookmarks or Starred pages deleted the highlighted entry, and on Wi-Fi it started a rescan. On Bookmarks, Starred pages and Wi-Fi a swipe now does nothing, while the OPDS catalog pages (see OPDS below). On other lists it still pages.
- **File Transfer, its hotspot, Calibre Wireless and USB Drive have an Exit button on a touch screen**, below their status. It does what **Back** does. USB Drive shows it only until a computer has the card, as it does Back.
- **Fix: tapping Back at the bottom of File Transfer and the hotspot did nothing.** On a reader without a Back key, such as the T5 S3 or the X4 Pro, a held home key was the only way out.

### OPDS

- **OPDS catalogs can be paged, and their buttons do what the hints say** (#374). Hold **Left** / **Right**, or swipe, to page through a catalog; **Up** / **Down** move one row, and two quick taps jump a page. A short **Left** is **Search** and a short **Right** is **Info** for a book. Where those do not apply, the hint shows only « or », instead of "Up" / "Down" labels on buttons that did nothing. Holding **Back** returns to the Home screen, and holding **Up** / **Down** jumps to the first / last entry. A long catalog in landscape no longer runs off the bottom of the screen. The side Up/Down hints also appear when choosing a download format, so the T5 S3 can move up there.
- **The OPDS Browser moved from Home into File Transfer**, beside Calibre Wireless, since it too downloads books onto the card. It is listed while at least one OPDS server is set up, and **Back** from it returns to File Transfer. A finished book's "Find more by this author" still opens it directly.
- **Fix: downloading a book again no longer deletes the copy you have if the download fails.** A download now goes to a temporary file and replaces the old one only once it is complete, so a cancelled or broken download leaves the book on the card as it was. This applies to font downloads too.
- **Fix: a book with several authors in a catalog shows its first author**, not the translator or illustrator listed after it. The name also goes into the downloaded file's name.

### Buttons

- **Every question is answered the same way.** Check for Updates, Clear Cache, Screen Repair, Wi-Fi's "Save password for next time?" and KOReader sync's "Upload current position?" each drew their question their own way and were answered with **Back** and **Confirm**. Like every other question, they now show two on-screen buttons and are answered with the two front buttons their hints name: Cancel (or No) first, then the answer that goes ahead (**Update**, **Clear**, **Start**, **Yes**, **Upload**). **Back** answers Cancel on every question, including the ones that ignored it, such as removing a book; on the X4 Pro and the T5 S3, so does holding the Home key.
- **Wi-Fi's saved-network options are a question like the others.** The **Cancel / Reset info / Forget** choice, offered from **Options** on a saved network or after one fails to connect, was drawn as text with a bracketed selection, and on a touch screen tapping an answer did nothing. It now shows three on-screen buttons, one above the other, and explains both actions at once. **Back** answers Cancel, and the two front buttons answer **Reset info** and **Forget**, as their hints say; you no longer move a selection and press Confirm.
- **Removing a folder that isn't empty says what it takes with it:** "Delete this folder and everything in it?" instead of "Delete?". **Switch to USB Drive** asks with a **Restart** button, and no longer tells you to press a Confirm key that doesn't answer it. Every question now carries its own question mark in each language, so French and Spanish punctuate it their own way ("Supprimer ?", "¿Borrar?").
- **Lists that wait for a double press no longer lag behind Left and Right.** Outside a book, the file browser, the pickers and the chapter lists waited 0.3 s after every Left/Right press to see whether a double press would follow, because the double press is bound to a reading action by default. Two quick presses then moved nothing at all in the file browser. The wait now happens only in the reader, where the double press means something.
- **Button hints fit their labels.** The hints along the bottom and beside the side keys were fixed-width boxes, so longer labels, in Russian or Ukrainian for example ("Вправо / Вкладка"), were cut off or ran over the next box. Each box now grows as far as its label needs and the screen allows.
- **Fix (X4 Pro): the side Up/Down hints sit on the edges the keys are on**, Up on the left and Down on the right, as on the X3, instead of both on the right.

### Lists

- **Settings, the reader menu, the pickers and the menus move the same way as the OPDS catalog.** Up/Down and Left/Right move one row; hold Left/Right to page a screenful (Left/Right used to page at once under "Up" / "Down" labels in the menus); hold Up/Down to jump to the first / last row, or in Settings and the reader menu to switch tabs; tap Up or Down twice quickly to jump a page. A page jump keeps the selection on the same line of the screen. A swipe now moves the selection with the page. The side Up/Down hints appear on all of these lists, so the T5 S3 can always move up.
- **Holding Up / Down to switch tabs in Settings and the reader menu opens each tab where you left it**, rather than at the top.
- **Holding Back on these lists returns to the Home screen** (it used to act like a short press), without losing changes: Settings saves first, and the reader menu closes the book as its **Go Home** item does. Inside a book, lists opened over the reader menu and quick overrides, and a file's options menu, still treat a held Back as a press.
- **More screens move the same way:** File Transfer's mode choice, the OPDS servers and their settings, Customise Status Bar, the font manager, Reading Stats' book list, the weather city search results and the finished-book screen. They show the side Up/Down hints, page with a hold of Left/Right or a swipe, and keep the selection's line when they do.
- **On/off settings on Customise Status Bar and the finished-book screen are switches** instead of "Show"/"Hide" and "On"/"Off" words.
- **A weather city search with no matches now says so** instead of returning to the settings without a word.

### KOReader sync

- **Send Document Metadata also sends the book's ISBN, Amazon ASIN, series and number in the series**, when the book gives them, beside its file name, title and authors, for self-hosted servers that use them. They go under the same names crosspoint-reader uses (`isbn`, `asin`, `series`, `series_index`). The setting is still off by default, and KOReader's own server ignores the extra details.

### Updates

- **Check for Updates shows what a new release brings.** Before you choose **Update**, the dialog now shows the release's title and the first few lines of its release notes under the version numbers, instead of the version numbers alone.

### Reader menu

- **Sync tab: Compare is a visible entry.** Next to Pull and Push, the Sync tab now offers "Compare progress with other devices", the same compare-then-choose flow a long Confirm press starts in the reader.

### Home and sleep screen

- **Fix: a book's details no longer run out of its card on the Home screen** (#375). On Lyra, a long title with several authors and a series ran over the selection highlight at both ends. When the card is short of room, the parts now give way one at a time: the reading-history line first (to its short form, then entirely), then the series, then the author, then the title, each cut with "…". Classic had the same fault at the Large UI font size and is fixed the same way.
- **Fix: a custom PNG sleep image is shown when the device sleeps straight from a book** (#377). It gave way to the default sleep screen, because memory the book had used was still taken. An image that cannot be shown also no longer sends you to the default screen: the next one is tried, up to three in all.
- **The sleep screen's info overlay counts pages as the status bar does.** In a book that splits a chapter into several files, it started again at page 1 in each file. It now shows the chapter page and total, and the printed page number, that the status bar showed when you closed the book.

### Keyboard

- **The tips under the text field stay still while you type.** The first letter typed added the "Hold DEL to clear all text" line and moved every tip half a line; that line's place is now kept free while the field is empty. "Hold UP to edit entry" is always shown, instead of appearing for a few seconds after two presses of Delete. Where the tips do not fit between the field and the keys, as in landscape, they are left out instead of drawn over both.

### Wi-Fi and the web interface

- **A new look for the web interface.** Every page now shares one design, with the same menu along the top and a button that switches between automatic, light and dark colours. The Settings page has a menu of categories and a search box, and changes are saved together with **Save changes**. Reading Stats, Diagnostics, the Font Manager, Dictionaries and Plugins fit a phone's screen without scrolling sideways. The new [Web Interface Guide](docs/webserver.md) shows every page.
- **Correct a book's details from the File Manager.** The new **Details** button beside every book edits its title, author, how the author is sorted, language, series and description, and sets its cover from a file, a pasted image or a search of Open Library, Goodreads or Google Books. It works for EPUB, XTC, TXT and Markdown books, and writes files beside the book without changing the book itself. It was the Metadata Editor plugin; a copy installed as a plugin keeps working until you remove it.
- **A text book shows the title its details give it.** On Home and in Recent, a TXT or Markdown book with a metadata file beside it is listed under that file's title and author instead of its file name, and edits to an XTC, TXT or Markdown book's details show there too, not only an EPUB's.
- **A new cover shows everywhere at once.** Saving a cover from the web interface makes the reader draw its thumbnails again, which on a reader without a set clock, and in some Home themes, kept the old cover until the book's cache was cleared.
- **A Plugins page in the web interface.** Plugins add tools to the File Manager and Settings pages, such as finding duplicate books or sorting books into a folder per author. The new **Plugins** page lists the installed ones, switches each off or on and removes it, installs new ones from this project's catalog (the browser needs internet for that) or from a `.zip` file, and shows which internet sites each plugin may contact through the reader. Before, a plugin had to be copied onto the card by hand, and nothing in the interface said plugins existed.
- **The web interface's pages are kept on the SD card, which makes the firmware 68 KB smaller.** The first time you open the reader's address, and after an update that changed the pages, a setup page installs them. On your Wi-Fi network it downloads them from the release by itself, in a few seconds. In hotspot mode the reader has no internet connection: choose the `web-….bin` file from the release page, or copy it to the top folder of the card. The setup page can upload books on its own as well, so a missing web interface never stops you adding books. It stays available at `/setup`.
- **Fix: anyone on the network could download your saved Wi-Fi passwords while File Transfer was running.** The web interface checked only the last part of a file's path, so the files in `/.crosspoint` holding the Wi-Fi, OPDS and KOReader passwords could be fetched by name, and the plugins' write requests could overwrite anything in that folder. Every part of a path is now checked. The three password files, and anything in the card's system folders, can never be downloaded or changed. Folders whose names start with a dot, such as `/.crosspoint`, can be opened only when **Show Hidden Files** is on, as the file list already showed them, and a folder or file whose own name starts with a dot can never be downloaded, overwritten, renamed, moved or deleted. Over WebDAV, nothing inside a dot folder can be downloaded or changed, whatever that setting.
- **Back leaves File Transfer during a slow upload.** A font upload, or a book the Files page sent the slower way (its fallback when the quick connection fails), kept the screen from responding to Back until the upload finished, which on a slow connection could take minutes. Back now cancels such an upload and deletes the part that arrived.
- **Fix: a refused font upload deleted the font installed before it.** An upload rejected at the start, for a bad file or family name, removed the file of the previous upload.
- **Install dictionaries from the web interface.** The new **Dictionaries** page takes the StarDict dictionary you downloaded, as a `.zip`, `.tar.gz` or `.tar.bz2` archive or as its loose files, and does what used to need a computer: it unpacks the compressed index the reader cannot use, offers to unpack the definitions too (lookups then need less memory), checks that the dictionary is one the reader supports, and puts it in a folder of its own under `/dictionaries`. An archive with several dictionaries becomes one folder each. The page also lists the installed dictionaries, says what is wrong with any the reader cannot use, and removes them. You still choose the dictionary on the device, under **Settings → Reader → Dictionary**.
- **A Diagnostics page for bug reports.** It shows how the reader last slept and started, the history from **Settings → System → Boot Diagnostics**, in plain words, and **Download report** saves it with the firmware version and device status as one file to attach to an issue, instead of a photo of the screen. The report leaves out the reader's IP and MAC address. Every page links to it, and so does System Info.
- **The File Manager asks before an upload replaces a file of the same name**: **Replace**, **Keep both** (the upload is saved as "name (2)"), or **Skip**, for that file or for every file of the upload. Re-sending a book from Calibre or a script still replaces it, as before.
- **The EPUB optimizer splits long chapters.** **Optimize EPUB** in the upload dialog now cuts a chapter of more than 8,000 words or 32 KB into several files, at a heading or scene break where it can, and points every link and table-of-contents entry at the right part. Long chapters are what make a chapter slow to open and run the reader short of memory while it is laid out. It also removes empty chapter stubs, comments and invisible characters, refuses books with DRM instead of breaking them, can keep the cover in colour, and can remember its options. Ported from CrossInk by Julia Nguyen.
- **Optimize books that are already on the card.** Select them in the File Manager and click **Optimize**: each is downloaded, optimized in the browser and put back, with the original kept beside it as `name.epub.bak`. A book whose chapters did not need splitting keeps its reading position and bookmarks.
- **Convert your own fonts in the Font Manager.** Choose a font family's TrueType or OpenType files, and the browser makes the reader's `.cpfont` files and installs them, without internet. The conversion is the one the catalog's fonts are made with, so a converted font looks the same as one from the catalog; the page used to send you to an outside website for this. It can also take the fonts a book embeds: **Details** in the File Manager has **Convert its fonts** for an EPUB.
- **The optimizer sizes pictures for every reader.** **Auto** used to know only the X3 and X4 and fell back to the X4's screen for the others; it now uses the screen of the reader you are connected to.
- **Fix: one click on Move, Rename or Delete in the File Manager could open its dialog several times**, once for every time the list had been sorted or refreshed.
- **Fixes for web-interface plugins:**
  - A plugin's requests through the reader now follow a redirect only to a site the plugin is allowed to reach.
  - A file a plugin saves is no longer cut short at the first zero byte.
  - A plugin that needs something this firmware lacks is refused with a message instead of failing.
- **A saved Wi-Fi password that stopped working is asked for again.** When the network turns the saved password down (it was changed on the router, say), the reader used to report only a timeout. Now the keyboard opens headed **Saved password may be wrong**, and a password that works can be saved over the old one. On the automatic connect, the reader first tries your other saved networks in range.
- **Fix: after a failed connect, the reader kept trying in the background.** It went on retrying the network with the error already on screen.
- **Saved Wi-Fi passwords work after the card moves to another reader.** Passwords are stored scrambled with the reader's own key, so on another reader, or from a copied `wifi.json`, they came out wrong and the network turned them down. They are now read with the key of the reader that saved them, and stored again under the new reader's key. A card that an older firmware has already opened on the other reader cannot be recovered this way: type those passwords once more.
- **On a mesh network the reader moves to a nearer access point.** When it joins a network with several access points, it sometimes joined a distant one and stayed there, with slow and failing uploads. After a connection with a weak signal it now listens for the network's other access points and moves to one at least 10 dB stronger, and only once it has heard that one twice. If the move fails, it reconnects as before.
- **Fix: a lost Wi-Fi link no longer freezes File Transfer.** The screen stopped responding until the reader was switched off. The link now gets 30 seconds to come back, with **Back** working throughout; if it doesn't, the reader leaves File Transfer. Calibre Wireless shows "WiFi lost, reconnecting..." meanwhile.
- **Calibre Wireless transfers faster.** Redrawing the progress after every change kept the SD card waiting for the screen most of the time; the progress is now redrawn every 4 seconds. **Back** also leaves Calibre Wireless during a slow transfer now.
- **Fix: a failed upload left a book cut short on the card.** When the card filled up partway through an upload, the part that had arrived stayed on the card and showed up in the Library. It is now deleted.

### Languages

- **Every language is complete.** Catalan, Czech, Danish, Finnish, Hungarian, Kazakh, Lithuanian and Romanian each gain about 500 strings that were shown in English, and Turkish and Vietnamese about 150. Where crosspoint-reader's translators had already translated a string, their wording is used, and each language follows their terms (in Catalan, for example, the informal address and "font"). About 740 strings we had translated before take their newer wording too, which also fixes missing spaces before a network name or after a label, typos, and keyboard tips that named the buttons in English. In Catalan, Czech, Finnish, Kazakh and Turkish the EPUB font size setting was labelled as the menu font size; in Swedish "None" read "No open book". The strings only this firmware has (the Library, Diagnostics, plugins and others) were machine-translated and have not been reviewed by native speakers yet: corrections are welcome.
- **The Library is translated.** Its tabs, views, book counts and menu entries, the **Compare progress** entry, the TXT/MD font settings and the slider hint now appear in every language the reader offers, instead of in English. In Belarusian, Dutch, French, German, Italian, Polish, Portuguese, Russian, Slovenian, Spanish, Swedish and Ukrainian the menus are fully translated again.

### Fixes

- **Fix: a chapter number and title that the book sets on separate lines no longer run together** (#388). Some publishers, St. Martin's Press among them, put the chapter number and title in one heading and use the book's style sheet to give each its own line. The reader ignored that and showed "10 The Tempest" on one line. It now shows the number centred above the title, with the spacing the book asks for. Every line of a heading the book splits this way also stays centred and at heading size, not just the first.
- **Fix: a KOReader sync from the start of a chapter now opens at the start of that chapter** (#268). It sometimes opened a dozen or so pages in. The reader replaced the exact position KOReader sent with an estimate from KOReader's percentage whenever the two disagreed by more than 1% of the book, and they often do, because KOReader counts rendered pages and the reader counts text. An exact position is now always used. The log also shows how each synced position was placed, so a wrong landing can be diagnosed from a user's log.
- **Fix: KOReader sync now decides which side is further by chapter and paragraph, not by percentage.** Smart sync, the sync on closing a book, the sync on waking, and the choice between the two ways of identifying a book all compared KOReader's percentage with the reader's. The two count differently (KOReader counts pages, the reader counts text) and often disagree by a chapter's worth, so a position at the start of a chapter could count as behind the end of the previous one: the reader then uploaded over a further position, or skipped the upload after reading on. The chapter now decides first, then the paragraph on the page, and the percentage only when neither can. The sync on waking also no longer scans the chapter just to compare; it does so only if it then uploads.
- **KOReader sync now lands on the exact page, in both directions, for every page that starts in text**, for every book, including those converted by Calibre. The reader records where each page starts in the chapter's text and syncs that: a position pushed from the reader names the text at the top of the page to the character, and a position pulled from KOReader opens on the page that text is on. Before, both directions estimated the page from how far into the chapter's text the position was, which on chapters with headings could land a page ahead (skipping text) and on dense ones a page or two behind, and books converted by Calibre could not be pinned to a page at all. A page that starts with an image lands on the text after the image. Chapters are laid out again once after the update. Pushing is also quicker: the reader no longer writes the chapter to the card and reads it back twice.
- **Fix: no more empty "( )" after a link with inline footnotes on.** When a link's target held nothing but spacing, the reader showed that spacing as the footnote, in brackets. In *The Sea Captain's Wife* it followed the number and title of chapters 1–9, whose contents entries pad the number with typographic spaces.
- **Fix: the Font Manager no longer runs out of memory reading the font list**, and neither does the fonts page of the web interface (where running out of memory restarted the device). The list is now read a piece at a time instead of all at once.
- **The font list now takes about half the memory it did**, and it is given back in one piece before a download starts. This makes room for downloads; it does not change the download itself.
- **Fix: GitHub downloads pass the certificate check again.** GitHub's download servers moved to a new Let's Encrypt certificate chain whose root key is 4096-bit RSA, which the reader's TLS library could not verify. The font list, font files and firmware updates (which never skip the check) failed with a certificate error; the font manager only worked with **Skip HTTPS validation** switched on.
- **Font downloads fetch each file in small pieces when memory is tight**, so the X3 no longer runs out of memory on GitHub's large secure-connection records partway through a font file (the X4 uses the same method). On those readers a file is now requested in pieces of 1 to 6 KB, sized to the memory that is free, over the same connection; a dropped connection picks up where it stopped. Book downloads from OPDS catalogs use the same method; a server that does not support it sends the whole file as before. The T5 S3 and X4 Pro, with more memory, download as before.
- **Firmware updates download in small pieces too, and are checked before they install.** On the X3 and X4 the update now fetches the firmware the same way as font files. Before the reader switches to the new firmware, it compares the download with the SHA-256 checksum GitHub lists for the release, and refuses an incomplete or corrupted download ("Download corrupted (checksum mismatch)") instead of installing it.
- **The font manager and the firmware update free more memory before they download**: the settings lists behind them, a loaded SD-card font, the bookmark list and cached scaled glyphs. Both restart the reader when you leave, as before, which loads everything again.
- **Fix: opening Settings could restart the device** when memory was fragmented, as seen on an X3. Every settings row was built into one 11.5 KB block at once, by the settings screen, every settings save and load, and the web interface's settings page; the rows are now built one at a time. A save made when memory was short could also write a cut-off settings file, which brought back every default on the next start. Such a save now keeps the old file.
- **Fix: a screen could take several seconds to draw.** When the processor slowed down to save power at the moment a screen began to draw, the whole screen was drawn at the slow speed: a frame of 0.2 s took over 3 s.
- **Fix: custom sleep images saved by GIMP or ImageMagick show in black and white.** These programs write a longer BMP header, and its extra fields were read as the image's colours, so black and white came out as two greys and grey images lost their darkest shades.
- **Fix: a wrong KOReader sync address could restart the reader.** A server address that answers with a large web page instead of the sync service filled the memory with it. Only the start of a reply is kept now, enough to tell that it is not a sync server.
- **Fix: Boot Diagnostics no longer shows crashes that never happened.** A wake that was refused (a power button released too early, say) was drawn in the history as a boot, with its reason read as "panic" or "int-wdt".
- **Fix (X3): the battery gauge is given the battery's real capacity.** A fuel gauge that lost power came back assuming a 3,000 mAh battery, four and a half times the X3's 650 mAh, and learned its readings against that, so the battery percentage could be off. When the reader finds the gauge in that state, it now loads 650 mAh into it, in the first seconds after start-up.

### More memory to work with

- **Leaving a book gives back the memory its glyph caches used**, 7 to 12 KB that every screen after a book used to run without, until the device slept or used Wi-Fi.
- **The list of recent books is loaded only while it is used**, on Home and in the Library, instead of from start-up on. That alone kept 4.5 KB and split the largest free block before a book was even opened.

### With thanks to

Where the code here is someone else's, it is credited in the source file it lives in. Where the idea is theirs and the code is ours, the source says that too.

- **Sung-jin Brian Hong** (@serialx): the SD-card font kerning fixes (crosspoint-reader #3838); a separate cache for the card's allocation table, and the cache fix after a failed read (#3685); log lines no longer lost at the low-power clock (#3737); and finding the memory churn in list navigation (#3698, #3848) and the reader's unlocked chapter reads (#3652).
- **Julia Nguyen** (@uxjulia, CrossInk): the EPUB optimizer's chapter splitting and clean-up, and the fixes behind the BMP palettes, failed uploads, OPDS downloads and authors, and the KOReader reply limit (CrossInk e092f81da, 7d49b69fa, 99712ef66, 1d954ae34, 234675e5c, 56a29f618).
- **Tuan Q. Nguyen** (@martinqnguyen): loading the X3's battery capacity into its fuel gauge (crosspoint-reader #3730, Free-Ink/freeink-sdk #132), and leaving File Transfer with Back during a slow upload (#3732, with **Uri Tauber**).
- **Uri Tauber** (@Uri-Tauber): the keyboard tips that stay still (crosspoint-reader #3863), the idea of storing font sizes in points (#2720), and giving the build's version only to the files that use it (#3860).
- **Justin Mitchell** (@itsthisjustin): closing the password files to the web interface (crosspoint-reader #3824), and the smaller secure-connection records and resumable downloads in the `freeink-sdk` that the chunked downloads build on.
- **suhsoohong** (@suhsoohong): keeping Back after a link across a reopen (crosspoint-reader #3764).
- **Phạm Bình An** (@brianhuster): finding why a long paragraph lost its top spacing (crosspoint-reader #3875).
- **The crosspoint-reader translators whose wording this release uses**, collected there by **Uri Tauber** (@Uri-Tauber, crosspoint-reader #2740, #2946, #3396):
  - Belarusian: **Dexif** (@dexif)
  - Catalan and Spanish: **Leopoldo Pla** (@lpla); Spanish also **mvidelatraduc**
  - Czech: **Jan Kaláb** (@Pitel) and **Bastian** (@dasrecht)
  - French: **Spigaw**
  - German: **BlueDragon** (@BlueDragon333), **Bastian** (@dasrecht) and **Wilhelm Schuster** (@wlhlm)
  - Hungarian: **Poc031205**
  - Italian: **Matteo Scopel** (@matteoscopel) and **luca** (@alan0ford)
  - Kazakh: **fsocietyipa**
  - Lithuanian: **nieko-nera**
  - Polish: **th0m4sek**
  - Portuguese (Brazil): **Rodrigo Matsuura** (@Rodrigo-Matsuura) and **Rodrigo Lanes** (@rodrigolanes)
  - Portuguese (Portugal): **CookieCaptainD** and **Tiago** (@tiagofilipenunes)
  - Russian: **winst0niuss** and **muhas**
  - Slovenian: **Andrej Kralj** (@thehijacker)
  - Swedish: **Stefan Blixten Karlsson** (@steka)
  - Turkish: **metoli86** and **Barış Albayrak** (@barbarhan)
  - Ukrainian: **KymAndriy**, **Mirus** (@mirus-ua) and **winst0niuss**
  - Vietnamese: **Hoang Manh Linh** (@jetaudio) and **Phạm Bình An** (@brianhuster)
  - A few strings come from crosspoint-reader feature PRs, translated by their authors (Nick Perez, Justinian, Oscar Nogueira Neto, Arthur Tazhitdinov, Julia Nguyen, WuTofu, HgGamer).

  All of them are listed in [docs/translators.md](docs/translators.md).
- The crosspoint-reader developers, whose list screens File Transfer's mode choice, the OPDS server list and editor, Customise Status Bar and the font manager's list are adapted from.
- The reports behind the fixes and features above: **PiperKev** for the KOReader sync landing (#268) and the chapter headings (#388); **syfq91** for the OPDS buttons (#374) and the Home card overflow (#375); **trungtypo-png** for the custom sleep image, with the fix they proposed (#377); and **WaRoarr** for the request to offer an SD-card font's own sizes (#395).

### Upgrade notes

- **The web interface installs its pages on your first visit after updating.** On your Wi-Fi network this happens by itself. For hotspot mode, have the `web-….bin` file from the release page ready (see Wi-Fi and the web interface).
- **KOReader sync and optimized books.** Optimizing a book in the web interface splits its long chapters, so its chapters are numbered differently from the original file. If you sync with KOReader, read the same optimized file on every device, or turn off **Split Long Chapters** in the upload dialog's options. See the [Web Interface Guide](docs/webserver.md#optimizing-epubs).
- **Every chapter is laid out once more the first time you open it**, because headings, long paragraphs, poems and pictures are laid out differently and each page now records where its text starts for KOReader sync. Each book's stylesheet is also compiled again once. Your place in every book is kept.
- **Each book is read once more for its details** the first time Details, Covers or Authors shows it, because the details now include the author's sort name. A book shows its filename for a moment while that happens.
- **The cover grid's thumbnails are made once more**, now whole instead of cropped, the first time a cover grid or the finished-book screen shows them. On the T5 S3 they are made at the new three-by-three size, and again whenever you change the UI font size.
- **The first visit to New or Authors builds the library index.** A book that has never been opened takes about a third of a second to read, so on a large card of unopened books this takes a while. It is kept from then on.
- **Home's Browse files and Recent books switches become one Library switch**, on if either of them was on, and Home's OPDS Browser entry moves into File Transfer.
- **Font sizes are now stored in points.** Your sizes are converted on the first start and kept. If you go back to an older firmware, it resets a size of 9 pt or more to the default, and reads a smaller one as a different size.
- **Catalan, Spanish and Vietnamese gained a few labels** from crosspoint-reader before the full translation, and German's message for a chapter with no content now reads "Leeres Kapitel" instead of "Kapitelende" ("end of chapter").

## 2.37 — 2026-09-29

Everything since 2.35, including the 2.36 pre-release. The on-screen keyboard now types Cyrillic, French, German and Spanish, and reading statistics no longer slow down leaving a book. The page counter counts the whole chapter, and a book whose layout changed reopens at the paragraph you were reading.

### Reading

- **The page counter counts the whole chapter** (#325). Some books split a chapter into several files. J-Novel Club light novels, for example, start a new file at every illustration. The counter used to start again at 1 in each file while the title still named the same chapter, so every picture showed as "1/1". The counter and the Chapter progress bar now run across all of the chapter's files. A file that has not been laid out yet is estimated from its size, and the total shows a `~` until the rest of the chapter has been prepared in the background. The status bar then updates without waiting for a page turn.
- **A book whose layout changed reopens at the paragraph you were reading.** Your place used to be kept as a page number and scaled by the chapter's new page count, which could land several pages away in a chapter with pictures, tables or headings. This happened after changing a setting outside the book, and after every update that re-indexes books. The reader now also saves the paragraph when you leave a book, and uses it whenever the chapter's page count has changed. If nothing changed, you return to exactly the same page, even one that starts mid-paragraph.
- **Chapters open faster in books with a great many files.** Every chapter's caches used to sit in one folder, and the card searches a folder from the start every time it opens a file in it. For a book with 1,732 files, that meant reading about 540 blocks of the card to open one chapter. The caches are now split into folders of 32 chapters, and the same open reads about 14. The sleep screen also finds the printed page number for late chapters of such books, which it used to miss.
- **The reader menu's tabs are plain text**, like the tabs in Settings, and while the tab bar is selected, the Confirm hint names the tab it switches to.
- **Fix: a double press or long press to jump pages left the old page on screen** (#351). The position moved, but the screen did not, and the next press then jumped one page past where it should have. This happened within a chapter, on the X3 and X4.
- **Fix (X4): Left and Right in the reader menu worked only every second or third press.** Screens opened from the reader (the menu, the contents, the printed-page dialog) were treated as the reader itself when a press was released. A press was held back to see whether it would become a double press, and was then lost. The same mistake swallowed reader shortcuts on those screens, such as the double press (±10) in the printed-page dialog in landscape, and touch gestures bound to reader actions.
- Fix: **Refresh Screen** now refreshes the screen you are looking at. Over the reader menu and the other screens opened from a book, it did nothing. The TXT, Markdown and XTC readers ignored it. On other screens it briefly showed the previous screen.

### Reading statistics

- **Remove a single book from your reading statistics**, without wiping the rest. The book's screen in Reading Stats has **Remove**, behind a confirmation, and each book on the web dashboard's statistics page has a remove button. The book's time, sessions and days come back out of the totals, as if it had never been read. Your longest streak is kept. Deleting a book from the card still leaves its history alone.
- **Leaving a book is instant again.** The statistics were kept in a single file that was rewritten in full at the end of every reading session. With a hundred books in it, that took 2.7 seconds on the X3 every time you left a book or the device went to sleep from one. They now live in a file with a fixed slot per book, and only the book you read and a small index are written: about 30 ms. A power cut during a save loses at most that one update, never the history.
- **Fix: a reading session could be lost** once about 25–30 books had statistics. The end of a session loaded the whole history into memory, and past that size the load failed and the session was not recorded.
- Fix: the web dashboard's statistics page and the statistics export could fail for a long history, because they also loaded all of it at once. Both now read it from the card as they go.

### Keyboard

- **Keyboard layouts for your language**: ЙЦУКЕН for Russian, Ukrainian, Belarusian and Kazakh, AZERTY for French, QWERTZ with ä, ö, ü and ß for German, and Spanish with ñ. Ukrainian ґ and the extra Kazakh letters are on a long press, since there is no room for more keys. Choose which layouts you use in **Settings → System → Keyboard Layouts**. Once two or more are on, a globe key switches between them. One Latin layout always stays on, because web addresses and passwords need one.
- The cursor and Delete now move by whole characters. Before, editing a title with accented letters could cut a letter in half.
- The keyboard comes from the `freeink-sdk` this firmware is built on, and works a little differently: Shift applies to one letter only, `-`, `=`, `.` and `,` moved to the symbols page, the keys look different, and on the T5 S3 tapping the text field places the cursor there.

### Home, settings and navigation

- **The home screen picks up changed book details.** A metadata file next to a book (`Some Book.opf`, as written by the metadata editor plugin) sets its title and author. The home screen and Recent Books only read it when the book was opened, so a change made later, over USB or from the plugin, did not show until then. They now notice the change and pick it up. Removing the file brings back the book's own details.
- **System Information is split into pages** now that it no longer fits one screen. The page buttons move between pages and the header shows "1 / 2". Pages follow the orientation.
- Fix: in **SD Firmware Update**, the button that opens the Options menu had no label (#329). The menu also offered things that do not belong there: Flash firmware (which Confirm already does), Move to folder, New folder and deleting folders. It now offers sorting, search and Remove on a firmware file, so an old firmware file can still be deleted on the device.
- Fix: in **System Update**, the front Up button did not wrap from the first item to the last (#333).
- Fix: in landscape counter-clockwise, the side button labels ("« Page", "Page »") were upside down (#336).
- **Fix: a city found by the weather location search could not be selected** (#342). The result list moved the cursor through invisible rows. The cause also made every list move one row by itself when you came back from a screen opened from it, for example after cancelling an entry in Weather Settings.

### The screen and fonts

- **Fix: a gap before the letter "i" in the menus.** In the 12 pt menu font, every "i" had a gap before it and none after it. When the fonts were made, the hinting that sharpens letter stems also nudged about one letter in ten sideways, by up to a pixel. The built-in UI fonts, Bookerly and Noto Sans have been regenerated, so letters sit evenly between their neighbours. The width of each letter is unchanged, so books do not re-paginate.
- **Fix (X4): entering Settings from Home could leave parts of the Home screen showing through.** The fast refresh compared the new screen with an older screen instead of Home, and skipped the pixels the two had in common. The same fix went into two other places with the same gap: the first chapter indexing of a newly opened book, and serial file transfer when memory runs low.

### With thanks to

Where the code here is someone else's, it is credited in the source file it lives in. Where the idea is theirs and the code is ours, the source says that too.

- **Justin Mitchell** (@itsthisjustin): the keyboard built on the SDK (crosspoint-reader #2481, with **Julia Nguyen**).
- **winst0niuss**: the set of language layouts and the key that switches them (#2858, with **Uri Tauber**), and moving the cursor by whole characters (#3094, #3095).
- **Julia Nguyen** (@uxjulia): the look of the keys (#3755).
- **Ankit**: Shift in web-address fields (#2357).
- **@thiagokokada**: sending Refresh Screen to the screen on top (#2683).
- The bug reports and requests behind the fixes above: **syfq91** for the chapter page counter (#325), the firmware picker (#329), System Update (#333) and the upside-down side labels (#336); **a1exb0nd** for the weather city search (#342); and **biggy-spoon** for the page jump (#351).
- The German and French letters and the smaller keyboard tables have been offered back to the `freeink-sdk` (Free-Ink/freeink-sdk #127, #128).

### Upgrade notes

- **Every book re-indexes each chapter once, the first time you open it**, because the chapter caches have moved to new folders. Your place in every book is kept. The old caches stay on the card and take up space until the book's cache is cleared. **Settings → System → Clear Reading Cache** removes them all at once, and also re-indexes every book.
- **Your reading statistics are converted once**, the first time they are used. That takes under a second, even for a hundred books. The old file is kept as `reading-stats.json.imported`. If you go back to an older firmware, it starts with empty statistics. Renaming that file back to `reading-stats.json`, or importing the statistics export from the web dashboard, restores them.
- Reopening at the paragraph works for books you leave after updating. A place saved by an older version still uses the scaled page number once.
- **Keyboard layouts start as your interface language's layout plus English.** In English and the languages without their own layout, the keyboard is English only, with no globe key, until you turn on more in **Settings → System → Keyboard Layouts**.
- The new labels are translated in German, French, Spanish, Italian, Dutch, Portuguese (PT and BR), Polish, Russian, Ukrainian, Belarusian, Slovenian and Swedish. "Keyboard Layouts" is translated in every language. Otherwise Turkish, Vietnamese and the eight partly-translated languages show the new labels in English until a native speaker fills them in.

## 2.35 — 2026-09-27

Everything since 2.31. You can organise and search the library on the device itself, pictures are decoded at their full resolution, and lists and headings are laid out the way the book's stylesheet asks. Underneath all of it, building a chapter takes far less memory than it did.

### Library and home screen

- **New Folder, Move to folder, and deleting a folder**, all from the file browser's options menu. Until now, a card with a few hundred books could only be tidied on a computer. Move opens a folder picker. It moves the file into the folder you are *browsing*, which the header names in full (`Move to folder: /Books/Tolkien`), and you can make a new folder from inside the picker, so a book can go somewhere that does not exist yet. A move is instant whatever the size of the book, since nothing is copied. It never overwrites anything: if a file with that name is already there, the move is refused and the reason shown on screen.
- **Search**, also in the options menu. **Search** narrows the current folder to names containing what you type. **Search all folders** looks in every folder below this one and lists the matches with their paths, and **Go to folder** opens the folder a result lives in, with that file selected. Back ends a search. The card is searched when you ask rather than indexed in advance: the card cannot report when its contents changed, so an index would go out of date after every USB copy.
- **Choose what sits on the home screen** (Settings → Display → Home screen). There is one switch each for Browse files, Recent books, Reading stats, Bookmarks, OPDS browser, File transfer and Weather. Anything you switch off moves behind a **More** row above Settings, so nothing becomes unreachable. Everything is on by default. Settings itself has no switch, because it is where you undo the choice.
- **Reading Stats is on the home menu.** Time per book, streaks and the day-by-day record used to sit three levels down in Settings, where the people they were built for never found them.
- **Each book's reading history is shown under its cover**, for example "Read 3h 18m over 5 days · last 6m ago". It counts the days a book was opened on rather than the number of times it was opened, since a ten-second glance is not a reading session. Each theme shows as much as it has room for. On the carousel, the line would run into the icon row on the X3's shorter panel or at the Large UI font size, so there a short form goes into the cover's badge instead. If the device's clock has never been set, the day count is left out rather than shown as zero.
- The History card in Reading Stats now says **Last finished** rather than "Finished". A book you finished and then started again no longer shows "Finished 3d ago" next to "52%".
- **Loading covers no longer gets in the way** (Lyra Carousel). If you press a button while a cover is being decoded, the decode pauses and later resumes where it stopped, instead of starting again from the beginning. Moving the selection while covers load now redraws in a few milliseconds instead of about a third of a second. Each cover is decoded once for both carousel sizes rather than twice; a 1.3 MB cover used to take two decodes of four and a half seconds each.
- Fix: on the Classic theme, with all seven home entries showing, the cover showed "Loading..." forever. The layout shrinks the cover tile to make room for the menu, but the tile still asked for a thumbnail at full height, a file that was never written.
- Fix (X4 Pro): the file browser's menu (Delete, Info, Mark as read, Set as sleep cover and the sort options) could not be opened at all, because it needed a long press of a key the X4 Pro does not have. On devices without a Confirm key, Confirm now opens the menu, and folders get an **Open** row.
- Fix: the first time you opened a book, the "Indexing" notice could appear over an older screen, such as the boot screen or Settings, instead of over Home.

### Pictures

- **Progressive JPEGs are decoded in full.** Until now they were shown from their first pass only, at an eighth of the resolution enlarged back to size, which is why they looked soft. In one illustrated novel, 77 of the 78 pictures are progressive JPEGs, and the text inside its diagrams could not be read. They are now decoded completely, one band at a time, so a large picture never has to fit in memory at once. The same applies to home-screen thumbnails and sleep covers made from a progressive cover: a 221×324 cover used to become a 27×40 smear stretched to fill the tile.
- **Percentage widths no longer shrink pictures below their real size.** Publishers wrap figures in boxes such as `width: 60%`, sized for a tablet screen, where 60% still leaves a picture close to its native size. On an e-reader's narrower page the same rule turned diagrams into thumbnails. A percentage box can still enlarge a picture, but it no longer shrinks it below its native width. Fixed-width boxes, pictures the publisher sized directly, and pictures with text flowing around them are unchanged.
- **Thin lines survive being scaled down.** Shrinking a picture used to keep one pixel from each block and drop the rest, so a one-pixel line disappeared whenever it fell between the kept pixels (35 of the 86 lines in one test drawing). The pixels are now averaged instead.
- **Pictures on the next few pages are prepared while you read.** Between page turns, the reader decodes the pictures on the next few pages, and on the first pages of the next chapter, and stores them for later, without changing what is on screen. On the X3, turning onto an illustration used to mean waiting 2–6 seconds for it to decode; a picture prepared in advance now appears in well under a second. If you turn the page while a picture is still being prepared, the work is saved to the card and picks up where it stopped next time, instead of starting over. Before, the X3 threw away more of this work than it finished. A "Large image — press Confirm to load" placeholder on the page you are reading now loads by itself after a short pause.
- **Fix: pictures showed only their alt text, and stayed that way** (#249). Photos exported from Photoshop or Lightroom carry up to 30 KB of camera and colour metadata before the part that gives the picture's size. Reading that far needs 32 KB of memory in one piece, which the reader almost never has while it builds a chapter, and when the memory was refused, the chapter's cache recorded the picture as broken. These headers are now read in stages from a smaller buffer, or later, when the screen's second frame buffer can be borrowed for the job. A chapter laid out without them is rebuilt once their sizes are known.
- Fix: a large image loaded with Confirm went back to its placeholder every time you returned to the page, because its cache was never saved. The check that decides whether there is enough memory to save it was still asking for far more than saving now needs.
- Fix: if a reset interrupted a picture while it was being unpacked from the book, that picture stayed blank on every later visit, because the half-written file was treated as complete. A picture is now saved under its real name only once it is complete, and a file of the wrong size is unpacked again.

### Layout

- **Lists are indented the way their stylesheet says.** A list's own margin and padding used to be ignored, and each item got a made-up indent instead. An item with `margin-left: 0` switched that indent off entirely, leaving bullet lists flush with the text, while a list the book had deliberately set to no indent was indented anyway. A list with no padding of its own still gets the usual default indent, as it would in a web browser. Lists also get their spacing above and below now.
- **A bullet no longer sits alone on its own line** when the item's text is inside a paragraph (`<li><p>…`). This is common: it is what Pandoc produces.
- **Spacing below an element now comes after the whole element.** When a chapter heading was split across two lines with a line break, the space it asked for below itself landed between its two lines. A box around two paragraphs put its space between them, and a figure put it between the picture and its caption. The space now goes after the element's last line.
- **The second line of a split heading is at heading size.** In `Chapter 1<br/>Setting Sail`, a very common way to write a chapter heading, the second line came out at body-text size. An empty heading also no longer enlarges the paragraph after it.
- `<section>`, `<article>`, `<aside>` and `<main>` now get their margins, and text inside an `<aside>` no longer runs on from the end of the paragraph before it. The paragraph before a list or section also keeps its own first-line indent instead of picking up the container's margin.
- **Books converted from PDF now show their pages properly.** A scanned book converted to EPUB carries its recognised text as an invisible layer over each page image, and that layer was being drawn: one such book came out as 1,206 pages, mostly one word per line, instead of its 154 scanned pages. Text made invisible with a transparent colour, `opacity: 0` or `visibility: hidden` is no longer drawn. Building those chapters also needed 180 KB of memory at its peak, more than an X3 has, because every word carried its own style. It now needs 74 KB.
- **Portuguese hyphenation.**
- Fix: a page with more than 16 footnotes left the extra ones out of its footnote list; the limit is now 64. Fix: a book with more than 1,500 stylesheet rules failed to process its styles and reread them all every time it was opened. Fix: in a document nested more than 64 elements deep, formatting could end on the wrong element.

### Reading

- **A chapter chosen from the contents appears as soon as the page you asked for is built**, instead of after the whole chapter is built. The page you asked for is usually ready a fraction of a second into the build: on an illustrated chapter on the X3, the wait behind the "Indexing" notice went from 7.6 seconds to 0.65. Following a link works the same way.
- **Fix: with auto page turn on, the device could fall asleep in the middle of a book** (#293). Auto page turn now holds off the sleep timeout while it is turning pages, and does nothing else: the processor still slows down and rests between turns, which is where most of an idle device's power saving comes from.
- **A small hourglass in the middle of the screen now confirms that a press was received** when it starts a slow screen change. Leaving a book, opening one, or moving in and out of settings can take a second or more, and with nothing on screen in the meantime, a press looked ignored and people pressed again. The hourglass is shown only on devices whose screen can draw it quickly. On the T5 S3, where even a quick refresh takes over a second, it did more harm than good, so it stays off there. You can turn it off anywhere with **Settings → Display → Show Busy Indicator**.
- Fix: jumping ten pages with a double-click during a slow page could save the new page as your place while the old page stayed on screen for several seconds. Jumping to a contents entry inside the current chapter could do the same.
- Fix: closing the reader menu could leave the menu on screen until the next page turn. The first redraw after the menu closed was treated as preparing the next page in the background, so it drew nothing on screen.
- Fix: the reader prepares the next chapter in the background while you read, but the once-a-minute clock update threw that work away, so a chapter that took longer than a minute to prepare rarely finished. The clock now waits for it.
- **Fix (X3): opening a heavily illustrated chapter could restart the device.** Even when it did not restart, every page for the rest of the session could be drawn with the slower refresh and with anti-aliasing off. Building such a chapter temporarily frees the screen's second frame buffer and works in that space, but a few things the build kept afterwards stayed in that space, so the buffer could not be taken back. On top of that, the diagnostic meant to report this kept the processor busy long enough to trip the watchdog and restart the device.
- **Fix: reading statistics could be wiped.** If the statistics file could not be read because memory was short, it was treated as an empty history, and the end of the next reading session saved that empty history over the real one. Now the file is left alone and read again later. A damaged file is set aside rather than overwritten, and the file is only replaced once a new copy has been written in full, so a power cut during a save keeps the old one. The statistics also have a size limit now; before, they grew with every book ever opened.

### The screen

- **Fix (X3): the sleep cover showed the outline of the last page read.** After a page drawn with anti-aliasing, the grey edges of the letters stay on the screen even after the display controller's memory has been reset to black and white. The cover was drawn only where it differed from that memory, so those grey pixels were never refreshed. After an anti-aliased page, the sleep cover now does one short clearing refresh first. Reading is unaffected.
- **Fix (T5 S3): ghosting built up over a long reading session.** The clean-up that runs after a grey page reset the panel's reference image from a buffer that had been lent to background work, which held an image that was never on screen. Each such clean-up made the next refresh update the wrong pixels.
- On the T5 S3, background work can now run while a page turn waits for the panel, so the next chapter is built while you read.
- Fix: on the T5 S3, and with 2-bit XTC books, leaving a TXT, Markdown or XTC book left its last page showing on top of the home screen until the next refresh.
- Fix: in lists whose rows wrap onto two lines (settings submenus, the language and option pickers, the reader menu), the list jumped by a row on every press near the bottom.
- **At the Large UI font size, text now stays inside its boxes.** Button hint labels ran out of both ends of their boxes into the neighbouring ones. They now switch to a smaller size before anything is cut off, because a whole word in a smaller size is still readable. The weather screen's text no longer piles up on top of itself.
- The clock and battery now sit in the same place on every screen. Recent Books, the file browser and its menu, Bookmarks and Starred Pages used to draw them further in from the edge.

### Clock

- **The timezone is now picked from a list of 86** (Settings → System → Clock Settings → Timezone). There are 49 named places with their own daylight-saving rules (among them Cairo, Tehran, Karachi, Dhaka, Kathmandu, Johannesburg, Nairobi, Perth, Brisbane, Phoenix, Mexico City, Bogotá, Santiago and St. John's, none of which could be chosen before), 37 fixed offsets for anywhere the list misses, and a plain UTC, which did not exist before.
- **Fix: Atlantic Canada, Australia Central and Alaska were reset to UTC on every restart**, even after Detect Timezone had set them. The screen that offered these zones and the check that validates a saved setting disagreed about how many zones there were.
- **Fix: "UTC" was actually UK time.** It followed British Summer Time, so a device that detected UTC ran an hour fast every summer. Lisbon and Dublin were detected as Central European time and were an hour out all year.

### Wi-Fi and sync

- **KOReader Automatic Sync** (X4 Pro and T5 S3). The device can fetch your reading position when it wakes, and send it every so many pages, when it sleeps, or when you close the book. This is for people who read on more than one device and rarely close a book. The options have their own **Automatic Sync** submenu in the KOReader settings, along with an optional sync indicator in the status bar, and everything is off by default. A background sync leaves Wi-Fi on if the screen you are using needs it. The X3 and X4 do not have enough memory for background sync, so they keep syncing only when you close a book.
- **Joining a hidden network.** The network list now ends with **Add hidden network...**. It asks for the network's name, then uses a saved password for that name if there is one, or asks for a password (leave it empty for an open network).
- **You can interrupt the automatic connection to a saved network.** Back cancels it and Confirm shows the network list, so you no longer have to wait for it to time out. Back during a manual connection also returns to the list; before, a mistyped hidden network name left "Connecting..." on screen for fifteen seconds.
- Leaving **Settings → Network → WiFi Networks**, a city search in the weather settings, or **Sync Time** now ends in a quick restart back to the same screen, as leaving every other Wi-Fi screen already did. Before, Wi-Fi stayed on until the device next went to sleep. That meant no idle power saving in the meantime, and the memory Wi-Fi uses was still taken when you opened the next book.

### More memory to work with

Most of this is invisible, but it is why much of the above works on an X3 at all. Building a chapter used to take its working memory piece by piece from the same pool everything else uses, while the screen's second frame buffer, 48–52 KB set aside for it, sat mostly unused. Building a chapter now works inside that buffer wherever there is room at the time, and hands it back afterwards instead of freeing it and hoping to get it back later.

- Building a chapter makes about a third as many memory allocations as before. The last round of this work alone cut the memory needed to build one illustrated chapter on the X3 from 32 KB to 21 KB.
- An illustrated chapter is built in one pass instead of two.
- On the X3, the home screen now keeps about 50 KB of memory free once the covers have loaded, up from 28 KB.
- Fonts on the SD card with wide character coverage, such as CJK fonts, no longer keep a separate identical copy of their character table for every style. That saves up to 48 KB per extra style, which was reloaded at every chapter change.

### With thanks to

Where the code here is someone else's, it is credited in the source file it lives in. Where the idea is theirs and the code is ours, the source says that too.

- **feyded1020**: organising the library on the device, with New Folder, Move to folder and deleting folders (#288), and search in a folder or across the card (#302); the reading-history line under each cover (#296) and Reading Stats on the home menu (#298); opening the file menu on the X4 Pro (#286); keeping text inside its boxes at the Large UI font size (#285, from their own report #281); putting the status bar in the same place on every screen (#287, #301); the Classic theme's cover stuck on "Loading..." (#295); "Last finished" (#299); catching a timezone mix-up before it reached a release (#289); and the diagnosis and design behind the busy indicator (#282).
- **Eric Kuck** (@EricKuck): KOReader automatic sync on wake, while reading and on sleep, with its status-bar indicator (#269).
- **Justin Mitchell** (@itsthisjustin): the timezone table and picker (crosspoint-reader #3562), and the follow-up that kept list scrolling working with touch (#3693).
- **Mr.Catfood** (@HgGamer): joining hidden networks (crosspoint-reader #2360).
- **Alexander Hoffer** (@axhoff): interrupting the saved-network auto-connect (#2189).
- **Zach Nelson** (@znelson): removing duplicate networks from the scan list without extra memory (#2262).
- **Phạm Bình An** (@brianhuster): lists with wrapped rows jumping near the bottom (#3668).
- **Julia Nguyen** (@uxjulia): pointing out that Wi-Fi stayed on after leaving the network settings (#3613).
- **Mauricio Juba** (@type0labs-dev): Portuguese hyphenation (#3643).
- **Eszter Schuffert** (@eszter007): sharing identical font tables across the styles of an SD-card font (#3616).
- **Sung-jin Brian Hong** (@serialx) and **Uri Tauber**: pinning the build toolchain so release builds stay reproducible (#3594).
- The bug reports behind two of the fixes above: **Kevin Palm** (@PiperKev) for #249, together with the book that showed it, which is how the cause was found, and **vntraam** for #293. Thanks also to the Discord report comparing one illustrated novel's pictures with another reader's, which led to the full progressive-JPEG decoder.
- The X3 sleep-cover fix and two display-driver fixes found along the way have been offered back to the `freeink-sdk` this firmware is built on (Free-Ink/freeink-sdk #114, #117, #118).

### Upgrade notes

- **Every book re-indexes its chapters once, the first time you open them**, because lists, spacing, split headings and picture sizes are all laid out differently. Your place in every book is kept. Each book's compiled stylesheet is also rebuilt once, which takes a fraction of a second.
- **Each picture is decoded again the first time you see it**, because pictures are now decoded at full resolution and their stored copies use a new format.
- **Home-screen thumbnails that were already made from a progressive-JPEG cover stay blurry** until they are made again. **Settings → System → Clear Reading Cache** remakes them, and also re-indexes every book.
- **Your timezone is carried over** and keeps exactly the same rules. If you never set a timezone, the device now shows London: it has been running on UK time all along, just labelled UTC. Pick your own zone, or the new plain UTC.
- **Reading statistics now keep the 100 most recently read books**, and each book's day-by-day record covers its last 60 reading days. Overall totals and your longest streak are kept. A statistics file that cannot be read is set aside as `reading-stats.corrupt.json` rather than overwritten.
- KOReader Automatic Sync is only available on the X4 Pro and the T5 S3.
- If you read in a language other than English, the first start after this update takes about a second longer while that language is unpacked into flash, as after 2.31. It happens once.
- The new labels are translated in German, French, Spanish, Italian, Dutch, Portuguese (PT and BR), Polish, Russian, Ukrainian, Belarusian, Slovenian and Swedish, and "Finished" in the reading statistics now reads "Last finished" in each of them. The Wi-Fi labels are also translated in the other languages, except that Danish, Finnish and Romanian show the three auto-connect labels in English. Otherwise Turkish, Vietnamese and the eight partly-translated languages show the new labels in English until a native speaker fills them in. The relative times on the reading-history line ("6m ago") stay in English in every language, as they already do on the Reading Stats screens.

## 2.31 — 2026-09-20

Everything since 2.30. Four larger reading sizes, a text size for the menus, and a firmware that came out smaller than 2.30 despite gaining both.

### Bigger text

- **Four new reading sizes: 20, 22, 24 and 26 pt**, available as the default size and as a per-book override. 18 pt was the ceiling, so a reader who cannot comfortably read it had nothing to move up to. 20 pt is a real typeface cut at that size — eight new faces, two families in four styles. 22, 24 and 26 pt enlarge the 20 pt master, because cutting all three for real would cost about 1.9 MB of flash the device does not have. The enlargement is area-weighted, and each distinct letter on a page is enlarged once and reused for the rest of that page.
- **Sizes are labelled by their point size** — 10pt through 26pt — instead of Tiny / Small / Medium / Large / X-Large. The adjectives ran out at "extra large", and they never said what they meant: "medium" was whatever 14 pt happened to be. Four places in the firmware kept their own copy of the list, two of which had gone stale, and one had them out of order — the picker read "12pt 14pt 16pt 18pt 10pt". The list is now derived from the single table that defines the ladder, so a size can only be added in one place. Your saved size is carried across, so every book keeps the size it was being read at.
- **Headings are sharper.** A heading is drawn by snapping its requested size to the nearest real face and enlarging whatever is left over. With the ladder stopping at 18 pt, a heading that wanted 25 pt took the 18 pt face and enlarged it by nearly half. There are rungs above it to land on now, so most headings are enlarged by a few percent or not at all.
- **A font installed on the card renders at every size too.** No `.cpfont` in existence ships anything above 18 pt, so the new sizes were unreachable on an installed font: the reader quietly put your typeface back to a built-in one. Your chosen typeface is now scaled from its closest face to the size you asked for, rather than being replaced.
- **Fix: changing the font or size from the reader menu did nothing until the book was closed and reopened** — and on a card font a size change did worse than nothing, silently swapping your typeface for the built-in one. Both symptoms, "nothing happens" and "it reverts to the default font", were the same bug seen from two sides: nothing re-loaded the font when an override changed, and the lookup that hands the font out accepts only an exact size match.
- Fix: a card font family that did not ship the size you asked for was read into memory in full and then not used.
- **New: Font Scaling Test** (Settings → System). Draws the whole ladder, 10 through 26 pt, for both built-in families, marking which sizes are real cuts and which are enlarged. It is here because the measurements cannot settle the question: they average out over a letter's whole area, and what a reader actually notices is edge definition and stem weight, which is exactly what enlarging trades away. So the decision rests on looking at it, on the panel, at its real resolution.

### The menus

- **New: UI Font Size** (Settings → Display: Normal / Large). Menu rows on a high-resolution panel are small and fiddly to hit with a finger. Large moves every menu, header and list row up one step. The reader's own status bar is deliberately left alone: it sets the text area a book is laid out into, and scaling it would re-paginate your whole library.
- Fix: in French, Brazilian Portuguese, Russian, Ukrainian and Belarusian, the setting that sets the size of the text *in a book* was labelled as the size of the text in the menus. It has always been the reader's size; the label was wrong, and with a real menu text size now sitting a tab away the two would have read alike.
- **Settings toggles are drawn as switches** rather than the words ON and OFF. Twenty-eight rows. Fix: Quick Resume on Timeout was the one on/off setting that kept showing text, because it was declared as a two-option list whose two options happened to be Off and On.
- **Every yes/no question has buttons on it now.** Clearing the cache, repairing the screen, confirming a firmware update and the general confirmation prompt all drew a centred question with nothing to press — the button-hint strip along the bottom was the only affordance, which on a device you are using with a finger is no affordance at all. The hint strip stays everywhere: it labels the physical keys, and on a device without a touchscreen it is the only affordance there is.
- **The reading-light pull-down is a drawer.** Swiping down from the top edge opened the Reading light settings screen — and opened it wrong: a screen titled "Reading light" whose single row was also "Reading light", costing a second tap to reach the controls. It is now a panel pulled over the page, only as tall as its contents, with the rest of the screen left standing.
- **Fix: a button press in a list could be missed entirely.** Moving the selection held the screen-drawing lock for the whole redraw, and buttons are read once per pass through the main loop — so a press that both started and ended inside that window was never seen at all. The longer the list, the slower the redraw, and the more presses went missing.
- Fix (touch): the first row of a list could sit hidden above a blank row until the next redraw, because the page size was worked out from a two-line row height where the list draws single-line rows. Seen in the reader menu's font size picker as soon as it grew to ten entries.
- **Fix: a partial repaint could draw on top of the previous screen.** Handing back the second framebuffer — which the home screen does while loading covers, and the reader does around building a chapter — threw away the frame that was actually on the panel, so anything that repainted part of the screen rather than all of it composited over a stale one.

### Speed

- **The font list is usable again.** Moving the cursor one row cost seconds: the preview re-read the font from the card, re-rendered the sample, and rewrote the whole 3.5 MB flash font cache — once per row. That last one also destroyed the selected font's cached copy, so merely opening the list made the next book you opened pay to rebuild it. Previews are now kept on the card and blitted back, about 120 ms a row with nothing allocated at all. On entry, any that are missing are built in one announced pass — "Creating font preview N/M", six families in around four seconds — and Back cancels it, after which the rest build as you browse.
- **Going in and out of a settings submenu no longer costs a two-second refresh each time.** The X3 clears accumulated ghosting with a slower refresh whenever a screen replaces something else, and settings asked for one on every return from a child screen, even when one had run three screens earlier and the panel was already clean. It is now spent only once several fast refreshes have gone by.
- Reads from the SD card transfer a sector in batches instead of one byte at a time — which is glyphs, chapter caches, cover images and the book itself.
- Trimming a label that is too long for its row is four to five times cheaper, and more than that the more there is to trim.

### Fixes

- **Fix: the web file browser could fail to reach Wi-Fi at all on an X3**, showing a plain connect timeout with nothing in the log to explain it. Joining a network is itself memory-hungry, and the radio was being brought up while a 52 KB framebuffer and the loaded font were still resident. Of the eight screens that use Wi-Fi, four freed that memory only after the join had succeeded, which is too late to help the join. The memory is now released at the point the radio comes up, which covers all of them.
- **Fix: a device left sitting in an OPDS catalogue never went to sleep**, and ran at full speed until the battery was flat. Browsing a catalogue, reading a book's details and looking at an error message are all screens you read, and they sleep like any other now; only the states with a transfer actually in flight keep the device awake.

### Room on the device

2.30 shipped with the firmware filling 96.7% of the space allotted to it — 219 KB spare on a 16 MB device. That margin is what a wireless update has to fit inside, and it was nearly gone. 2.31 adds an entire new typeface size and a second menu text size and still lands at 92.8%, with about 470 KB spare. Three changes paid for it, and none of them is visible while reading:

- **The other 23 languages ship compressed**, and whichever one you have selected is unpacked into a reserved slot in flash. Every language used to sit in the firmware in full, at all times, and a reader uses one. 232 KB.
- **Font data that describes the typeface rather than the size is stored once per family** instead of once per face — which characters it covers, the kerning classes, the ligatures. Across 55 faces that was 226 KB of byte-for-byte duplication.
- **Each letter's record is 6 bytes rather than 16.** Most of what was stored in it can be worked out from the letter's own width and height. About 480 KB, and looking a letter up is roughly 10% quicker as well, so this is not the trade it looks like.

### With thanks to

Where the code here is someone else's it is credited in the source file it lives in; where the idea is theirs and the code is ours, the source says that too.

- **Justin Mitchell** (@itsthisjustin) — the reading-light drawer's behaviour, and routing a held finger to a slider (crosspoint-reader #2983).
- **Jay Silverman** (@Techneaux) — the diagnosis behind lists dropping button presses (#3534).
- **Sung-jin Brian Hong** (@serialx) — giving a laid-out page's elements a single owner (#3518).
- **Uri Tauber** (@uritaube) — letting the OPDS catalogue sleep like any other screen (#3547).
- **Osakana Taro** (@osakanataro) — batching the SD card's SPI reads (#3501).
- The size ladder's idea — snap to a real face and scale only the remainder — is from **CidVonHighwind/microreader**.

### Upgrade notes

- **Chapters are re-indexed once per book the first time you open them**, because headings now reach the new larger faces and so break across lines differently. Your place in every book is kept.
- **Your reading size is carried over**, both the global one and any per-book override, even though the sizes were renumbered to put them in order.
- **If you read in a language other than English, the first boot after this update takes about a second longer** while that language is unpacked into flash. It happens once per update that changes any text, and not at all if none of the text changed. English is unaffected.
- **The first font load after this update is slower.** The flash font cache is rebuilt from the `.cpfont` files on your card, because the language slot now sits at the end of the same area of flash. Nothing you installed is lost — the flash copy was only ever a cache of what is on the card.
- The three new labels — UI Font Size, Font Scaling Test and the font-preview progress notice — are translated in German, French, Spanish, Italian, Dutch, Portuguese (PT and BR), Polish, Russian, Ukrainian, Belarusian, Slovenian and Swedish. Turkish and Vietnamese show the English text for those rows until a native speaker fills them in; the eight partly-translated languages are unchanged.

## 2.30 — 2026-09-16

Everything since 2.26. Two new devices, touch control throughout, and a long run of fixes to the boot and sleep paths.

### Two new devices

- **The Xteink X4 Pro and the LilyGo T5 S3 Pro now run this firmware**, alongside the X3 and X4. Both are ESP32-S3 devices with a touchscreen; the T5 S3 adds a backlight, and the X4 Pro a two-channel light whose warmth can be set separately. On both, the capacitive Home key is Confirm on a tap and Back on a hold. Touch is an addition, never a replacement: every button still does what it did, and all of it can be switched off.
- **Firmware is now published per device.** `firmware.bin` stays the combined X3/X4 binary under the name it has always had, so devices in the field keep updating as before; the new boards take `firmware-x4pro.bin` and `firmware-lilygo.bin`. The update check asks for its own device's file, and both the over-the-air path and the SD-card path now refuse an image built for a different device rather than installing it.
- Getting there meant unpicking an assumption that ran through the whole firmware: it asked "is this an X3?" and treated everything else as an X4. Pin assignments, the SD and display buses, the battery gauge, the clock chip, the panel controller and the display's own capabilities are now read from a per-device profile, so a fourth device is a profile rather than an edit in eighty places.
- Fix: the recovery firmware mode — hold Up and Power at boot — could not be reached on the X4 Pro or X4 Classic, because Up is the pin the chip samples at reset to decide whether to run the firmware at all. Holding it meant the firmware never started, which from outside is indistinguishable from a dead device: the documented escape from a bad SD card was the thing that looked broken. Those devices use Down instead.
- Fix (T5 S3): leaving the reader menu, and leaving a book, showed the old screen and the new one superimposed for a refresh. Fix (T5 S3): two cleaning passes in a row left both pages visible at once.
- Fix: on devices where the touch controller, the clock and the battery gauge share one wire, two of them could talk at once and corrupt each other — including a case where the display's power rails were raised while a frame was being clocked out.

### Touch

- **The whole interface answers to touch.** Rows, book covers, the home menu, the settings tabs, the on-screen keyboard, sliders, the dictionary's word picker, footnote markers and the button-hint strip along the bottom all respond to a finger. Lists use *point-then-confirm* — the first tap moves the highlight, a second tap on the same row opens it — so a mis-tap costs one more tap rather than an action to undo. If you would rather rows opened on the first tap, **Settings → Controls → Tap Action** switches it.
- **Twenty-odd gestures, each one assignable** like a button, under **Settings → Controls → Gesture actions**. Out of the box: tap the outer thirds to turn pages, tap the middle for the reader menu, hold left or right to skip a chapter, hold the middle to look a word up, hold the bottom to star the page, pinch to resize text, and turn two fingers to rotate the screen. Swipe sideways in a page-turn zone to jump ten pages. Swipe down from the top edge for the reading light, up from the bottom edge for the reader menu, and up or down at the left and right edges for brightness and warmth. **Settings → Controls → Gesture overview** draws the zones and your own assignments on the device itself, so it is always right even if the manual is not.
- **Hold the top-left corner to turn the reading light on and off, on every screen** — not just while reading. The corners answer only to a hold, never a tap, so no tap anywhere changed meaning; the other three are free for you to assign. Double-pressing Power does the same thing without touching the glass, which is the one that works in the dark.
- Two master switches, deliberately separate: **Touch Page Turn** governs the reading page (Off / Tap Zones / Swipe / Tap Zones Inverted) and **Touch Navigation** governs everything outside it. Turning off page turns so a resting thumb cannot flip a page should not also stop you tapping a book in the library. Neither can strand you — the reader menu stays reachable with page turns off, and Back and Confirm come in as button presses below the level these settings act on.
- The LilyGo T5 S3 has a Down key but no Up key, so paging a list *backward* has no button there: **tap the scroll bar** above the thumb, or swipe over the list. Both work on the X4 Pro too.
- Fix: taps were lost when the reader was busy or coming out of sleep, the carousel's side covers ignored taps, list rows were sized for an 800-pixel screen, and taps on the side button hints did nothing. Fix: a gesture that was refused said nothing at all, and "Built-in" did not say what it would actually do.
- On the X3 and X4 the touch code is compiled out entirely, so none of this costs those devices anything.

### Boot, sleep and wake

- **Fix: the device could hang on the way to sleep with the sleep screen showing and every button dead**, recoverable only with the reset pin (#155). The last thing the sleep path did was wait for the power button to be released, with no way out — a sticky switch, or a finger that never lifted, and it waited forever. It now gives up after five seconds and sleeps anyway.
- **Fix: an X4 could sit on the sleep screen ignoring the power button** (#155), which reads exactly like a dead device. Some X4 units do not latch their own power: they stay alive only while the button is physically held, and the firmware did not close that latch until well into the boot — after reading settings, checking the wake gate, three memory integrity walks and the update state. Anything shorter than all of that and the device dropped dead mid-boot with the sleep image still on the glass. The latch is now the first thing the firmware does.
- **Fix: one too-short press on the power button quietly powered the device off** instead of doing nothing. Refusing a press is a "nothing happened" answer, but on the X4 it was taking a path that cut the battery latch — downgrading the device from "asleep, wakes on a tap" to "off, needs a hold long enough to carry a whole boot", and losing the clock with it.
- **Fix (X3): the SD card stayed powered through deep sleep**, draining the battery, because the rail was only released on the X4 branch of the sleep path.
- **Fix: a deadlock while the reader was starting up**, plus a set of races and deadlocks between the drawing task and the main loop — including a shared file position used by two tasks at once, and the task watchdog being fed from a task that had never subscribed to it.
- **New Boot Diagnostics page** under Settings → System. It reports how this boot started, where the last sleep stopped, and pairs each sleep with the boot that followed it. The point is that "it failed to sleep" and "it failed to wake" look identical from outside, and almost nobody has a serial cable attached. One screenful, no scrolling — it is meant to be photographed into a bug report.
- Fix (X4 Pro): the main peripheral rail was dropped during deep sleep.

### The screen

- **Fix (X3): vertical banding on grayscale images.** The four-level image path staged its greys as a correction over a black-and-white base and resolved them with a short pulse — and that short pulse is what exposes the panel's own drive unevenness, showing as stripes every eight lines on anything dithered. Those images now carry the whole frame in one pass at the panel's full waveform. Measured upstream on the affected panel, the column variation drops from 3.5–4.7% of the black-to-white range to 0.9% and the eight-line pattern disappears. This covers the bitmap sleep screen and the book-cover sleep screen — a JPEG or PNG cover is decoded to a bitmap first, so it takes this path too — and grayscale images in the image viewer. Text rendering and the reader's own pages are untouched: the short pulse suits them.
- **Sleep images on the T5 S3 are drawn at the panel's real depth.** Every device dithered sleep images to four grey levels because that is all the display pipeline could carry, which is a hard ceiling on the older panels but not on this one. On a panel with more levels the picture is now decoded once and shown whole, and the "Equalize" filter is applied more strongly to match, since there is finally somewhere to put the extra range. On a bimodal cover that gains about six points of brightness and a fifth more local contrast.
- **New: Repair Screen** (Settings → System). Fast page refreshes deliberately skip the eraser, so ghosting builds up in a way the periodic clean-up does not fully clear. This drives every pixel hard between black and white several times and settles on white — about twenty seconds, nothing is deleted. It is a maintenance action, not a fix for ghosting while you read.
- **New: Screen Edge Margin** (Settings → Display: Narrow / Medium / Large). On the T5 S3 the case comes close enough to the live pixels that text at the edge is hard to read. This adds 0, 5 or 10 pixels on top of what the device declares. Narrow is the default and is exactly the old behaviour, so nothing moves unless you ask.
- **New: a "Lighter" step for text darkness**, in Settings and in a book's own overrides. Every existing step added weight and none took it off. It is modest — it moves the partly-inked edges of letters, not their cores.
- **Fix: an idle reader flashed the whole screen every fifteen minutes or so.** The ghosting budget is counted in pages, but the once-a-minute clock tick was spending it like a page turn, so the budget drained whether or not anyone was reading and a full refresh eventually fired for no reason but a changing digit.
- The "sunlight fading" toggle is now offered only on the devices whose glass actually fades, since enabling it costs panel time on every page.
- The clock in the status bar can sit at either end.

### Reading light

- The T5 S3's backlight is driven, on/off and brightness. On a two-channel light, warmth gets the same quick controls brightness has.
- **Brightness and warmth now preview live as you move the slider**, and Cancel puts back what you had. Choosing a light level used to mean picking a number, confirming, looking at the screen and going back in. If the light was off, previewing turns it on and then puts it back off — asking for a brightness is not asking for the light.
- Fix: brightness and warmth were not actually saved. Fix: one of the quiet background restarts could bring the light back at the wrong level.

### Reading

- **Fix: coming back from a footnote landed at the start of the chapter** instead of the page the note was on — and so did keeping your place across a font-size change. Positions are anchored to a paragraph so that re-flowing cannot move them, but the paragraph counter only saw paragraphs sitting directly inside the document body, and a large share of real books (anything Calibre has produced, for one) nest them one level deeper. Every page in such a chapter recorded "paragraph 0", which was then read as a real position meaning the top.
- **Fix: following a link could exit the book.** Back has only the saved position to return to, and a change intended to treat contents links as navigation rather than as a detour stopped saving one — so Back fell through and closed the book.
- **The footnote list now separates notes from navigation links.** A page's internal links were all recorded as footnotes, so a chapter's contents link sat among the real notes. Notes come first, then links, each marked and in page order. They stay in the list rather than being hidden, because without touch the list is the only way to reach a link at all.
- **Fix: hidden text was displayed.** Publishers use the HTML `hidden` attribute for answer keys, teacher's notes, alternate-language blocks and metadata that ships but must not show. The parser never looked at it.
- Fix: `!important` was stripped from only twelve kinds of CSS declaration, so the rest lost out to rules that should not have beaten them. Fix: a footnote reference styled on the link itself, rather than wrapped in a `<sup>`, was drawn full-size on the baseline. Fix: Korean text written as combining jamo was not composed into syllables.
- **Fix: looking a word up selected fragments rather than words** (#206). With bionic reading on, "reading" offered "read" and "ing" separately; a word hyphenated across a line break offered each half. Neither looked up. A word broken at its own hyphen resolves either way now.
- **Fix: an image could be replaced by its alt text and stay that way** (#249). Reading an image's dimensions is refused when memory is short, which is meant to be a statement about one moment — but on the main path the refusal was written into the chapter's cache, where it survived page turns and re-entry. Changing the font was what brought the picture back, which is how this was found. The reader now frees what it can and tries again before giving up.
- Fix: a chapter was rebuilt from scratch after a single failed allocation, and an SD-card font could lose an entire style — and so every letter on the page — for want of one block of memory slightly smaller than the one asked for. Fix: a partly-extracted cover is decoded rather than discarded.
- Fix: the header could clip a long label on the right. Fix: text inside nested tables was dropped. Fix: a failed archive read was treated as an enormous one.

### Library, menus and transfer

- **Settings and the reader menu are now tabbed** — Navigation, Settings, Sync and Tools in a book; Display, Reader, Controls and System outside it — instead of one long list. The design is adapted from **CrossInk** by uxjulia.
- **Fix: one press in the file browser ran two handlers.** Confirm entered a folder and then immediately opened the first thing in it; Back always left the browser however deep you were, because the short-press "up one folder" branch could never be reached.
- Fix: picking a firmware file from a folder with 64 or more files in it listed the books next to them, and picking one handed an EPUB over as firmware.
- Fix: a submenu could contain itself. Fix: a settings row did not repaint after being toggled. Fix: taps on weather search results did nothing.
- **New: USB Drive on the X4 Pro and LilyGo T5 S3.** The card appears on a computer as an ordinary removable disk, so books go on and come off with the file manager. On those devices it replaces USB Transfer, which had nothing left to offer once a computer can mount the card directly.
- **Security fix: the web file browser could be walked out of its protected folders.** The rules that keep dotfiles, `System Volume Information` and the cache folder off-limits look only at the last part of a path, which is only sound once `..` has been resolved — and six handlers never resolved it. Download, delete and upload were all affected, and an uploaded file name carrying its own separators landed wherever it liked.
- Fix: Wi-Fi went into power-saving during OPDS browsing, book downloads and KOReader sync, which is where it hurts most. Fix: a screenshot over the cable arrived truncated.

### Under the hood

- Images and covers are noticeably quicker: a stored PNG is decoded where it lies instead of being unpacked first, both pixel caches are written from one pass, writes to the card are buffered, the cover's metadata parse is remembered, and the extracted-image cache is keyed so it survives a layout change.
- A chapter's anchors are streamed to the card rather than held in memory — the single largest chapter-sized allocation in a build — and the footnote preview index now lives on disk, which lifted the cap that made books with more than about five hundred notes give up part-way through. Style lookups skip work that could never match.
- SD-card font glyphs are read straight from the flash mapping instead of being copied into memory a page at a time, which is what was competing with the image decoder above.
- Fix: the home screen retried a 48 KB allocation that could not succeed, once per redraw, for the rest of the session.

### With thanks to

Much of this release is ported from, or built on, other people's work. Where the code is theirs it is credited in the source file it lives in; where the idea is theirs and the code is ours, the source says that too.

- **uxjulia** (Julia Nguyen) — USB mass storage, from the firmware activity down to the USB handoff (crosspoint-reader #3203, freeink-sdk #36/#53/#57), with Justin Mitchell and Uri Tauber; the footnote-reference styling fix (#3355); and **CrossInk**, whose icon-above-label tabbed reader menu this release's menus are modelled on, and whose guide-dots reading aid and table column sizing were earlier borrowings.
- **Bryan O'Sullivan** (@bos) — the diagnosis, encoding and measurements behind the X3 grayscale banding fix (crosspoint-reader #3469; freeink-sdk #94/#95).
- **Justin Mitchell** (@itsthisjustin) — per-device firmware assets and the wrong-device guard (#2880, #2983).
- **Uri Tauber** (@uritauber) — freeing dictionary font caches on exit (#3317); firmware download links on pull requests (#3389).
- **Sung-jin Brian Hong** (@serialx) — Hangul jamo composition (#3036); retrying the glyph arena smaller instead of dropping the style (#3126).
- **Sylve** (@s0lness) — found and fixed the web file browser path traversal (#3353).
- **Joe Harpham** (@jjharpham) — the HTML `hidden` attribute (#3390).
- **Foulad** (@sfoulad) — the failed archive read (#3244); Wi-Fi power-save during downloads (#3252).
- **Phạm Bình An** (@brianhuster) — `!important` on every declaration (#3221).
- **Jadehawk** (@jadehawk) — keeping Wi-Fi awake through KOReader sync and authentication (#3233).
- **adiskill** — reaching the reader menu with touch page turns off (#3319).
- **FDKevin** (@fdkevin0) — the clipped header label (#3235).
- **YUN-JIE** (@YuunJiee) — moving the settings category table into flash (#3364).
- **Antoine Aflalo** (@Belphemur) — holding the X4 Pro's peripheral rail through deep sleep (#3215).
- **Oliver Freyermuth** — memory plotting for the S3 devices.
- **jetaudio** — panel, light and sleep bring-up for the T5 S3 (freeink-sdk #51).
- The screen repair sequence is modelled on `repair()` in **azw413/lilygo-t5s3paperpro-rs**, an independent driver for the same panel family.
- The `freeink-sdk` this firmware sits on is maintained upstream by the Free-Ink project. This release takes several resyncs with it — the largest carrying forty-eight upstream commits — alongside the panel, light, USB and grayscale work contributed back the other way.

### Upgrade notes

- **Download the file that matches your device.** X3 and X4 keep `firmware.bin`; the X4 Pro takes `firmware-x4pro.bin` and the LilyGo T5 S3 takes `firmware-lilygo.bin`. An image meant for another device is now refused rather than installed.
- Chapters are re-indexed once per book the first time you open them, because hidden elements, `!important` and footnote-reference styling all change how a page is laid out. Your place in every book is kept.
- On the X4 Pro and the LilyGo T5 S3, USB Transfer is replaced by USB Drive. The X3 and X4 are unchanged.
- **Any gesture assignments you had customised are reset to the new defaults.** The vertical swipes changed meaning during this cycle — they are anchored to the left and right edge columns now rather than to halves of the screen — so a stored choice named a gesture that no longer exists. Button assignments are untouched.
- The reading light's on/off state is restored on wake only if **Restore Light on Wake** is set; brightness and warmth are always remembered.
- The new touch, gesture, reading-light and diagnostics labels are translated in German, French, Spanish, Italian, Dutch, Portuguese (PT and BR), Polish, Russian, Ukrainian, Belarusian, Slovenian and Swedish. Turkish and Vietnamese show the English text for those rows until a native speaker fills them in; the eight partly-translated languages are unchanged.
- See **[Touch Controls](USER_GUIDE.md#5-touch-controls)** in the user guide for the full gesture reference, or **Settings → Controls → Gesture overview** on the device.

## 2.26 — 2026-08-28

- **Buttons now follow the screen, not the panel.** The four front buttons and the two side buttons sit on different edges of the device, so rotating it swaps which axis each pair moves along — but navigation still assumed portrait. In landscape a list stepped with the buttons beside the screen and jumped pages with the ones below it, the two directions were often reversed, and the hint labels named buttons that did something else. Every list, cursor, keyboard and picker now works out which physical button points the way you mean for the orientation you are holding, and each hint is drawn beside the button that performs it. Page turns, yes/no prompts and your own short/double/long press assignments keep their fixed buttons, as before.
- Fix: held upside down (inverted portrait), the labels on the four front buttons were drawn upside down.
- **Fix: verse and quotations lost their first letters** (#198). Poetry is usually marked up with the indent on the stanza and the hanging indent on each line. Only the first line of a stanza ever picked up the stanza's indent, so from the second line on the text started off the left edge of the screen and its first letter was clipped away. Text could also be pushed off-panel by a negative margin, or by a hanging indent deeper than the indent it hangs out of. Two visible consequences: a line continued with a line break keeps its paragraph's indent instead of getting a fresh first-line indent, and a table inside an indented block is now indented with it.
- **Fix: sleep-screen covers came out nearly black with the "Adaptive" filter.** The filter corrected the picture's own tonal range and the reader then dropped the panel's brightness measurements, so it re-corrected on top — on a sample cover 87% of the image landed in the two darkest levels against 31% with the filter off. Book images were never affected; only the sleep screen.
- **New "Equalize" cover filter** (Settings → Display → Sleep Screen Cover Filter). "Adaptive" stretches a picture between its darkest and lightest points, which does nothing for a cover that is mostly dark with a small bright title — five of seven sample covers came out effectively unchanged. Equalize spreads the tones by how much of the picture actually carries them, so those covers gain real contrast. It is a stronger transform, so smooth backgrounds show a little more dither texture; both are worth trying on your own covers. Applies to inline book images as well.
- **HTTPS connections are now really verified.** Every https request checked the server's certificate and then quietly repeated the request without checking when that failed, so a bad certificate was simply accepted — including on the KOReader sync connection, which carries your sync password. Verification failures are now reported instead. To reach a self-hosted server with a private or self-signed certificate there is a new **Skip HTTPS validation (risky)** toggle under Settings → System; firmware updates stay verified whatever it is set to.
- Fix: signing in to or syncing with some KOReader servers reported a Wi-Fi login page ("captive portal") when the server was simply saying it had nothing stored for that book yet. Servers spell that several ways, and two of them — an empty JSON string and a bare `null` — were being read as a portal.
- **Checking the time is about three seconds faster.** The system clock's time request used to sit in a random delay of up to five seconds before the first packet left the device, on an exchange that takes a few hundredths of a second. The reader now asks directly, and only falls back to the old path if that gets no answer. This delay was paid on every sync, timezone detection and weather update that needed the clock set.
- Wi-Fi connects more reliably: the radio no longer dozes between beacons while connecting, which is what turned a weak moment into a ten-second dead attempt before the retry, and the channel scan is back at its longer dwell after a shorter one made the reader miss access points it could see perfectly well.
- **About 30 KB more memory free**, on a device with roughly 380 KB in total: the Wi-Fi driver's non-critical code is loaded from flash rather than kept resident, two system tasks had their stacks cut to what they actually use, and unused cloud-service components are no longer built in. More free memory means fewer of the restarts and slow rebuilds that a fragmented heap causes. The trade is lower Wi-Fi throughput during transfers, which is a fair swap for a device that syncs a page number and occasionally downloads a book.
- Fix: a PNG uploaded through the web file browser was converted to a JPEG on its way to the card (#196). Only JPEGs are re-encoded now, which is what that step was for.
- "Auto page turn" moved to the top of the reader menu, beside the other reading controls.
- Fix (X4): heavy ghosting after an anti-aliased page. The panel's charge pump was left powered between an anti-aliased page and the next black-and-white one, and could not clear the grey charge at reduced strength.
- Upgrade notes: chapters are re-indexed once per book the first time you open them, because indented blocks are laid out differently — your place in every book is kept. The two new setting labels ("Skip HTTPS validation" and the "Equalize" filter) are English-only in this release; the other 14 languages show the English text until the next translation pass.

## 2.25 — 2026-08-22

- **New: dictionary lookup.** Copy a StarDict dictionary into `/dictionaries` on the card, choose it under Settings → Reader → Dictionary, then look a word up from the reader menu or from any button you map to the Dictionary action. Left/Right steps word by word, the side buttons jump between lines, Confirm shows the definition. Definitions keep the dictionary's own headings and emphasis when there is memory for it and fall back to plain text otherwise. With more than one dictionary installed, Confirm on the definition screen switches to the next one and remembers the choice. Plurals, possessives, verb endings and a dictionary's own synonym list are tried automatically, and a hyphenated compound falls back to its parts ("multi-billionaire" → "billionaire"). A word the dictionary does not have is drawn as an outline before you press anything, so you can see there is nothing to look up. Pronunciation renders properly: Noto Sans now carries the phonetic alphabet, and a font that lacks a symbol draws a close lookalike instead of a row of boxes. Nothing is bundled — dictionaries are files you put on the card; see `docs/dictionary.md`.
- **Lists move faster.** Up and Down step one item; Left and Right now jump a whole screen and keep going while held, so crossing a few hundred chapters or files no longer means holding a button and waiting. Double-tapping Up or Down does the same jump on the screens whose Left/Right are already taken — the file browser, where Options has moved to a long press on Right and the long-press delete is gone (the context menu offers the same thing).
- Long titles in the file browser now wrap over up to three lines instead of being cut off. A folder holding one series used to show the same truncated text on every row, because the part that tells the books apart is the part that got cut.
- Fix: a double-tap on Up/Down registered as two single steps about nine times out of ten — a screen refresh between the two taps was enough to lose the gesture.
- Fix: on a list that fits on one screen, two quick taps threw the selection to the far end instead of moving two rows, and a third tap wrapped it back to the top.
- Fix: on the starred-books screens and the Wi-Fi list, one press did two things at once — it moved the selection *and* triggered the row's action (rename or delete, connect or rescan).
- Recent Books shows covers in a 2×2 grid instead of 3×3. At three columns a cover was drawn about 105×158 pixels, too small to recognise the artwork; the grid now works its size out from the panel, and covers are drawn at their stored size rather than scaled down, which also removes the faint grid pattern that rescaling left in them.
- Fix: a book whose cover is stored in a format the reader cannot decode (an AVIF named "cover.png", for instance) was unpacked from the book again on every pass through the shelf, about 1.3s each time — and a cover that failed also left the *next* book showing as coverless for the rest of the session. Scanning covers no longer writes a helper file to the card for every book it passes, either.
- Fix: opening Recent Books in cover mode could restart the device, as could a chapter whose cache was dropped before it was ever read.
- **Tables** (#186). Tables came out as loose text whenever a cell spanned some but not all of the columns — which is how Standard Ebooks writes row headers, so nearly every table in those books fell back. Three more table fixes came with it: a table that crosses a page break repeats its header row at the top of the next page, a `<caption>` no longer prints below the table it belongs to, and a table keeps its own indentation instead of always spanning the full width.
- Fix: tables could disappear from a chapter completely, and a chapter could be cut short and its shortened form cached, both because memory checks were set far above what the work behind them actually needs. The reader noticed the short chapter and quietly built it a second time, so this also removes duplicated work on table-heavy books.
- Fix: a KOReader or OPDS server address typed with a single slash ("https:/example.com") could never connect, and reported an unrelated redirect error after three timeouts — one slash and two are hard to tell apart on e-ink. Such an address is now repaired as you enter it, and one already saved is repaired at the next boot.
- Fix: the clock in the status bar overlapped the battery percentage when the battery read 100%.
- Fix: an X3 with a USB cable attached could produce no serial output at all for a whole session — the log you are asked for when something goes wrong. Plugging the cable in after boot now opens the log too, instead of needing a restart.
- Pages are built with about 7.5% fewer memory allocations, which leaves the heap less broken up over a long reading session.
- Upgrade notes: chapters are re-indexed once per book the first time you open them, because table cells now record how many columns they span — your place in every book is kept. If you install a dictionary, prefer a plain `.dict` over a `.dict.dz`: the compressed form needs a 32 KB unbroken block of memory that a long reading session cannot always spare (`dictunzip` on a computer converts it).

## 2.24 — 2026-08-20

- Fix: wide images were shown with a large black bar underneath, and other pictures came out stretched or squashed. Six separate problems in how images are sized and drawn are fixed, including a cover that looked right at the top and repeated a single line for the rest of the way down.
- Chapters full of pictures now open straight away. They used to spend up to a minute preparing every image in the chapter, including ones on pages you never reach.
- Fix: pictures could stop appearing for the rest of a book after a brief moment of low memory.
- Fix: some books would not open at all — every chapter failed. They open normally now.
- Fix: Cyrillic text was missing letters.
- Fix: "Normalize font size" had no effect on books that set their text size in a stylesheet. Those books stayed slightly off the size you chose and looked a little blurred. Headings still keep the size their publisher gave them.
- The first footnote in a book no longer sets off "Gathering footnotes" and then "Indexing". Notes are prepared one chapter at a time now, so jumping to a note and back is immediate, and books whose later chapters you never open no longer pay for them. Previews also read better — no stray link or heading text mixed in — and coming back from a note returns you to the paragraph you were reading rather than roughly the right page.
- The reader menu, table of contents, footnote list and bookmarks are about three times quicker to open and to move around in.
- Fixed two freezes that could only be cleared with the reset button: one after a long press — most often when starting a KOReader sync from inside a book — and one when opening the menu, contents, footnote list or bookmarks. Two unexpected restarts around footnotes are fixed as well.
- Fix: waking straight back into a book could draw the page on top of the sleep image instead of replacing it (X4).
- Fix: signing in to a KOReader server failed after the device had been fully switched off, with an error that gave no clue why.
- Fix: the clock on X4 lost time badly. Several ordinary actions — finishing a sync, leaving the web server, downloading a font, installing an update — restart the device quietly in the background, and each one set the clock back to when it last checked the time. It now keeps the time across those restarts.
- Wi-Fi now always connects to the strongest access point. It could previously pick a distant one when several share a network name, take six or seven seconds, or report "no access point found" and quietly try again. Connecting takes about three seconds and behaves the same way every time.
- Fix: on the newer X4 units with the updated screen, the display was upside down and quick page refreshes showed nothing at all.
- Reading statistics are only loaded when you open a screen that shows them, which leaves more memory free while you read.
- The firmware went from almost completely full to about 93% of the space available, mostly by storing the fonts more compactly. Nothing about the fonts themselves changed. This is what made room for the fixes above.
- Upgrade notes: chapters are re-indexed once the first time you open each book, because of the image and text-size fixes above — your place in every book is kept. Pictures are prepared again for the same reason, so the first look at an illustrated chapter is a little slower than the second. If your X4's clock has been badly wrong, it will put itself right the next time the device is online.

## 2.23 — 2026-08-16

- Fix: letters were unevenly thick within the same word (#149). The reader fonts were built without grid-fitting their stems, so an identical stem shipped as 3 pixels of ink in one letter and 4 in the next — at Bookerly 14 it split `b d f i n r u` from the rest of the alphabet. All 40 built-in reader faces (Bookerly and Noto Sans, 10-18pt, four styles each) and 24 of the 28 SD card font families are rebuilt. Letter *spacing* is untouched, so no book repaginates and no reading position moves.
- Waking from sleep is faster and less fussy. The boot no longer runs behind your finger — it used to stop and wait for the power button to be released, and since nothing on screen confirms the press was accepted, people held on, which is exactly what made it slow (measured on X4: 3.9s to a visible page instead of 6.8s). The hold needed to wake is also shorter, and a quick double-click now wakes the device, which it previously refused even though a double-click is what put it to sleep.
- Fix: reading could jump back to the start of the chapter (#147). The anchor a chapter rebuild resolves into was written once when the chapter was entered and then left frozen, so any mid-chapter rebuild — a background build aborting on low memory, a footnote gather, a failed page load — dropped you at the top of the chapter, and the next page render saved that position, so it survived a reboot. The anchor now follows the page you are actually on.
- Fix: a single failed allocation while turning a page deleted the chapter's cache and re-indexed the whole chapter. The cache file is almost always intact in that case; the reader now frees what nobody is waiting on and re-reads the same file, and only rebuilds if that fails too.
- Read-ahead and in-place chapter builds are admitted by a memory check that was asking for more than any build has ever needed, so it could never pass. The check is corrected; the safety margins themselves are unchanged.
- The SD card font "Readerly" is replaced by "Libron" from the same author under the same licence — Readerly was withdrawn upstream and could no longer be built. Its listing also claimed Cyrillic coverage it never had.
- Upgrade notes: the built-in fonts come with the firmware, but SD card fonts are files on your card — Settings → Fonts will now offer an update for every family you have installed, and the fix only applies once you take it. Readerly is no longer in the catalogue; an installed copy keeps working, but Libron is its replacement.

## 2.22 — 2026-08-14

- Background chapter build (read-ahead) now actually runs on device: its memory floors were unreachable — one by 12 bytes, another excluding every book with embedded CSS — correct builds were being discarded as "degraded", and background work ran at the 10 MHz idle clock. A section that took 28s to build now takes ~2s, the up-to-1.2s input freezes during a background slice are gone, and read-ahead waits 1.5s of stillness instead of 4s so it also works while skimming.
- Fewer restarts while reading: the fragmentation caused by a page render interleaving with a live chapter build is fixed (font page slots released, build arena adopted mid-build, arena-aware image decode floors).
- Added web-UI plugins loaded from the SD card (`/.crosspoint/plugins`), with four bundled: metadata-editor (edit title/author/series/description and manage covers, incl. Open Library / Goodreads / Google Books cover search), organize-by-author, find-duplicates and a reference plugin. Plugins run in the browser; a manifest declares which capabilities and remote hosts they may use.
- Added Calibre `book.opf` metadata sidecars: a `book.opf` beside a book overrides its embedded title, author, language, series and description without rewriting the EPUB.
- Added "Adaptive" as a fourth Sleep Screen Cover Filter, applied to BMP and full-screen PNG sleep images and to book covers, with line art detected and left alone.
- Inline images in books now render in grayscale (and get adaptive tone mapping) when anti-aliasing is on.
- KOReader sync: new "On Progress Conflict" setting ("Ask every time" / "Use furthest", changeable from the compare screen), automatic discovery of the document id the server actually uses (KOReader defaults to a content hash, we defaulted to filename — the two never saw each other), an optional sync when a book is finished, and a configurable auto-push page threshold.
- Idle light sleep: the chip now halts between button polls when idle, with unchanged button response. Light-sleep and deep-sleep behaviour is reported on the System Information screen.
- Faster boot (~0.45s off the recovery-combo check) and ~0.5s faster wake straight back into a book on X3.
- Sleep images are now decoded once and cached in a sidecar, instead of three or four times per sleep.
- Books with no real footnotes no longer pay a ~2.8s whole-book scan before the first page.
- Web file browser: images preview in-page instead of downloading, and dropping a folder now uploads its contents with the folder structure intact.
- Fix: an oversized table cell deleted itself and every earlier cell in the same table; tables now lay out row by row, which also removes the limits that made large tables abort a page build.
- Fix: the e-ink panel and the SD card share one SPI bus but only storage locked it, so an interleaved refresh and SD read could corrupt data or panic the system.
- Fix: a failed footnote gather recorded "this book has no footnotes" permanently, surviving reboots.
- Fix: finishing a book synced the start of the last chapter instead of the end.
- Fix: applying remote KOReader progress resolved the position correctly and then discarded it.
- Fix: KOSync now accepts any 2xx response from compatible servers.
- Fix: watchdog-triggered resets are now reported as crashes instead of rebooting silently.
- Fix: X3 reported USB connected and showed the charging icon when detached with a full battery.
- Fix: the web Settings page could stall or trip the watchdog by opening three simultaneous connections.
- Fix: replacing a book cover by hand had no effect, because the cached thumbnail was never invalidated.
- Fix: a finished book left its metadata sidecar behind when moved to /COMPLETED.
- Fix: sleep and wake button handling now agree with each other.
- Hardening: stored credentials (Wi-Fi, KOReader, OPDS) are decoded with a bounded buffer.
- Translations: the 13 strings that were still English-only (the new KOReader sync and sleep settings, the adaptive filter, "Normalize font size", NTP server and the new System Information rows) are now translated in all 15 fully-maintained languages — German, French, Spanish, Italian, Dutch, Portuguese (PT/BR), Polish, Russian, Ukrainian, Belarusian, Slovenian, Swedish, Turkish and Vietnamese.
- Upgrade notes: book covers are re-generated once (they are now cached as 8-bit greyscale so they can be tone-mapped), sleep images gain a small hidden `.pxc` cache file beside them, existing KOReader configurations keep "Ask every time" while new installs default to "Use furthest", a `book.opf` beside a book now takes precedence over the book's own metadata, and web plugins run unsandboxed on a web server that has no authentication — install only ones you trust.

## 2.21 - 2026-08-03
 - Recognition of newer X3 / X4 batches with alternative display controller
 - Implemented "Normalize font size" as a user setting that snaps publisher near-body font wrappers (e.g. around whole paragraphs) back to native 100%
 - Fix: The webserver settings screen could be mangled if the koreader password contained special character
 - Fix: Koreader server authorisation and registration could fail under heavy memory load
 - Fix: Font download could fail under heavy memory load
 - Fix: OTA update could fail under heavy memory load
 - Fix: The involuntary "wake on short power button press" has been resolved
 - Fix: Relative image sizing recognizes outer blocks

## 2.20 — 2026-08-02

- Major page-render speedup on styled books (most commercial EPUBs set a body font size): X4 page render ~450ms → ~50-100ms, X3 ~950ms → ~545ms.
- Page turns feel snappier — the anti-aliasing touch-up is now dropped when you navigate away mid-render instead of finishing it first.
- Fixed a freeze (up to ~28s) finishing a book that has sequels in the same folder.
- Faster library browsing, cover thumbnails, Wi-Fi/web file transfer, and opening of very large books.
- Fixed crashes/failures opening very large books (1700+ chapters) under low memory.
- Fixed a blank cover/title page on books with SVG-wrapped cover art.
- Added drop-cap support (large stylized first letters).
- Fixed button input on the "book finished" screen, dropped page-turns on side buttons, and a footnote-list double-skip.
- Left/Right buttons and labels now work correctly in the reversed (rotated) mounting orientation.
- X3: fixed the charging icon sticking on after unplugging, and the SD card not powering down in sleep (battery drain).
- More reliable Wi-Fi connects (DNS stall fix) and better AP selection when several share an SSID.
- Known issue: newer X3/X4 hardware batches with updated display panels aren't supported yet.

## 2.10.1 — 2026-07-17

- Fixed some EPUB3 books (common Calibre/Pandoc output) failing to open due to a metadata-parsing regression introduced in 2.10.

## 2.10 — 2026-07-17

- Reintroduced progressive JPEG support and reduced image-decoder memory use.
- Added guide dots and inline footnote previews from a book-level cache.
- Added configurable OPDS download folder/filename format.
- Fixed a crash entering deep sleep on the web-transfer screen, an AP low-memory crash on X3, and an out-of-bounds page bug on fast page-turns.
- Fixed ZIP scanning for large/fragmented EPUBs and reduced CSS-related OOM risk.
- Faster shutdown; ~3KB flash saved via zopfli compression.

## 2.09-freeink — 2026-07-14

- Switched to the official freeink display SDK (from open-x4-sdk).
- Fixed several X4 ghosting issues (popup and section-crossing).
- Added an optional X4 display overclock for faster screen refresh, and SDK/version info on the System Information screen.

## 2.09 — 2026-07-14

- Fixed a cover-thumbnail deadlock on a corrupted cached cover image.
- Fixed dither-band artifacts in JPEG decoding.
- Kept full CPU speed while home-screen covers are still resolving (faster library home screen).
- Fixed the web server blocking book listing for Calibre-connected devices.

## 2.08 — 2026-07-13

- Fixed a sporadic missing page update.
- Folded the X4 anti-aliasing pass into the page-refresh window (fewer/faster refreshes).

## 2.07 — 2026-07-09

- Cut first-open time on large books via buffered EPUB cache I/O.
- Fixed secondary-framebuffer release during first-open book indexing.
- Completed the Swedish translation.

## 2.06 — 2026-07-08

- Added support for different font sizes and improved font scaling/reuse.
- Fixed KOReader sync (KOSync) failing under low memory.
- Fixed an OOM reboot from loading the printed-page list all at once.
- Fixed sleep-screen cover Fit/Crop display issues.

## 2.05 — 2026-07-02

- Improved font upscaling quality.
- Added support for rendering large embedded covers on the sleep screen.
- Added an optional physical page-number display.
- Fixed several ghosting issues (indexing popup, X4) and a classic-theme list display bug.

## 2.04 — 2026-06-29

- Added a live "page X of ~Y" estimate while a section is still building.
- Added a book-keyed HTML cache to skip re-inflation on reopen.
- Fixed reader resuming at the chapter start instead of the saved position.
- Re-rendered UI fonts with a native monochrome rasterizer for crisper text.

## 2.03 — 2026-06-26

- Fixed tall images not being sliced properly.
- Fixed a display-refresh race that could cause ghosting.
- Hardened OTA update-metadata fetching; added low-memory safety nets.

## 2.02 — 2026-06-24

- Reworked the web server to release memory properly (stability).
- Fixed anti-aliasing/half-refresh behavior when the sunlight fix is active.
- Fixed a couple of settings submenu bugs.

## 2.01 — 2026-06-24

- Fixed an XML parser regression that could cause books to fail to open.

## 2.00 — 2026-06-23

- Added USB serial file transfer (MicroReader-compatible), with host-side Total/Double Commander plugins.
- Made serial downloads reliable (fixed dropped bytes on transfer).
- Added "more books by this author" lookup and a Power-button shortcut to select a footnote.
- Fixed CSS parent/child margin collapsing.

## 1.99 — 2026-06-18

- Added file-browser sorting (name/date/size/type) and an options menu; bounded-RAM browsing for very large folders.
- Added smallcaps support and persistent file timestamps (RTC-backed).
- Fixed a downscaling darkness issue and carousel-exit ghosting.
- Improved cover-thumbnail handling and background slicing for carousel covers.

## 1.50 — 2026-06-15

- Replaced the JPEG decoder (TJpgDec), fixing several JPEG/GIF decoding bugs and X4 ghosting issues.
- Improved rendering speed for image-heavy documents and CSS-heavy books.
- Reworked the recent-books grid view and fixed stale highlights in it.
- Unified the input handling for TXT/MD readers with the main reader.

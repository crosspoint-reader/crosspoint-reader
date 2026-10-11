# Sidecar files

A sidecar is a file sitting beside a book, sharing its name but with a different
extension. The firmware prefers it over the equivalent data embedded in the book,
so you can correct a cover or a title without rewriting the EPUB.

```
/Books/
    Some Book.epub      the book
    Some Book.jpg       cover sidecar     (optional)
    Some Book.opf       metadata sidecar  (optional)
```

The rule is the same for both: **a sidecar wins over what is inside the book.**

## Cover sidecar — `book.jpg`

Recognised extensions, in this order: `.jpg`, `.jpeg`, `.png`, `.bmp` (and their
uppercase forms). The first one found wins.

When present it replaces the book's embedded cover on the home screen and in the
Library, and the firmware skips extracting the embedded cover entirely.
Implemented by `ReaderActivity::sidecarCoverPath()`.

The File Manager's **Details** dialog manages this from the web UI: preview the
current cover, replace it from a local file, paste an image copied from any
website, search Open Library / Goodreads / Google Books, or remove it. Saving a
cover there also deletes the book's cached thumbnails, so every view, on any
clock, draws the new one.

**Replacing a cover takes effect on the next load.** The rendered thumbnail is
cached per book, and `ReaderActivity::ensureCoverThumb()` compares the sidecar's
modification time against it: a newer sidecar wins and the thumbnail is
regenerated. Note this depends on the clock — files written while the RTC is
unsynced all carry 1980-01-01, so on hardware without a working RTC a replaced
cover may need a cache clear. The check covers the box-sized thumbnails
(`thumb_<W>x<H>.bmp`) of the Library's Covers view and the Home carousel. The
single-height `thumb_<H>.bmp` the other Home themes draw is not compared, and
keeps the old picture until the book's cache is cleared.

## Metadata sidecar — `book.opf`

A Calibre-style OPF. Calibre writes one beside every book it exports, so a
library exported from Calibre already has these and needs no extra work.

Fields taken from the sidecar, when non-empty:

| Field | Source in the OPF |
|---|---|
| title | `<dc:title>` |
| author | `<dc:creator>` (every creator, joined for display) |
| primary author and its filing name | the first `<dc:creator>` with `opf:role="aut"` or no role, and its `opf:file-as` (EPUB 3: `<meta refines="#id" property="role">` / `property="file-as"`) |
| language | `<dc:language>` |
| series | `<meta name="calibre:series">` or `belongs-to-collection` |
| series index | `<meta name="calibre:series_index">` or `group-position` |
| description | `<dc:description>` |
| ISBN, ASIN | the first `<dc:identifier>` of each kind: `opf:scheme` naming it (`ISBN`; `MOBI-ASIN` or `AMAZON`), or a `urn:isbn:` / `isbn:` (`asin`) prefix on the value |

The File Manager's **Details** dialog writes these from the web UI, so you do not
have to hand-edit XML. It edits an existing sidecar in place rather than
regenerating it, so publisher, identifiers, dates and other fields it does not
manage survive untouched. It refuses to save a sidecar over the 16KB cap. For an
EPUB without a sidecar it reads the book's own OPF through `GET /api/book/opf`
rather than downloading the book.

A minimal sidecar:

```xml
<?xml version='1.0' encoding='utf-8'?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="uuid_id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:title>The Title As You Want It Shown</dc:title>
    <dc:creator opf:file-as="Lastname, First" opf:role="aut">First Lastname</dc:creator>
    <meta name="calibre:series" content="Series Name"/>
    <meta name="calibre:series_index" content="4"/>
  </metadata>
</package>
```

### Books other than EPUB

TXT, Markdown and XTC books take their title, author, primary author and series from a metadata
sidecar too, in the Library (the Details and Covers views, and the author the Authors tab files them
under). Without a sidecar, an XTC book is labelled from its own header, and a TXT or Markdown book,
which carries no metadata, by its filename. Language and description are taken for EPUBs only. The
reader itself still shows the book's own title. Every format goes through the same reader,
`MetadataSidecar::read()`, the book's OPF parser run over the plain file.

### Rules

- **Empty fields are ignored.** A sidecar supplying only an author leaves the
  book's own title alone, so a partial sidecar cannot blank out good metadata.
- **Cover references are not taken from it.** `coverItemHref` points inside the
  ZIP, which a sidecar cannot meaningfully supply — use a cover sidecar instead.
- **Only metadata.** The spine, manifest and reading order always come from the
  book itself. A sidecar cannot change what you read, only how it is labelled.
- **A malformed or oversized sidecar is ignored**, and the book opens normally
  with its embedded metadata. The cap is 16KB
  (`Epub::MAX_METADATA_SIDECAR_BYTES`); a real Calibre OPF is a couple of KB.

### When changes take effect

Immediately — on the next time the book's metadata is loaded.

The sidecar is deliberately **not** baked into the `book.bin` cache. That cache
is only rebuilt when the EPUB's own bytes change, so a baked value would leave an
edited sidecar silently ineffective. Instead the overlay is applied on every
load, including the cached path (`Epub::applyMetadataSidecar()`). Cost when no
sidecar exists is a file-existence check per extension (`.opf`, `.OPF`).

Deleting the sidecar restores the book's embedded metadata, again on next load.

The home screen and the Library's Recent tab are an exception: they show a copy
of the title, author and series kept in `recent.json`, taken when the book is
opened. So that a sidecar written later still shows without reopening the book,
each entry also stores a hash of the sidecar it was taken with
(`SidecarFiles::metadataStamp()`). When those screens open, any EPUB entry whose
sidecar hash has changed (sidecar added, edited or removed) has its metadata
reloaded (`RecentBooksStore::refreshSidecarMetadata()`). Hashing the content
rather than checking the modification time catches edits that keep the same
length, and works on a device whose clock was never set.

The Library's other lists keep a copy too, in `details.bin` in the book's cache
directory ([file-formats.md](file-formats.md)). It records the same hash, so a
sidecar added, edited or removed is read again the next time the book's row is
drawn. The New and Authors tabs read the book index, which records a signature of
each book's `.opf` (its size and date): when it changes, the next rebuild of the
index looks the book's author up again ([design/library-index.md](design/library-index.md)).
The index's walk pairs a sidecar with its book by the exact filename before the
extension and takes `.opf` / `.OPF` only, so a sidecar whose name differs from
the book's in case is read for the book's labels but gets no signature: editing
it does not change the author the Authors tab files the book under until the
index is refreshed (Options → Refresh library).

## Moving books

A sidecar is tied to its book by filename, so **moving or renaming a book must
carry its sidecars along** — otherwise the cover silently reverts to the embedded
one and metadata corrections are lost.

Who does this today:

- **Move to `/COMPLETED`** (the optional finished-book action) moves the book
  and every sidecar, resolving name collisions for each
  (`moveSidecarFilesToCompleted`).
- **Move to folder / Remove in the Library.** Its tabs list books only,
  so a sidecar is never a row of its own there. Moving a book (Books tab)
  takes its sidecars along, and the move is refused if one of them would land
  on an existing file (`SidecarFiles::anyTargetTaken` / `moveAll`). Removing a
  book (Books, New and Authors tabs) removes them too
  (`SidecarFiles::removeAll`); on the Recent tab Remove only takes the book off
  the list. **All Files** (Settings → System) lists sidecars as ordinary files
  and moves or removes exactly the file selected.
- **The `organize-by-author` plugin** moves sidecars with the book — see
  [sd-plugins.md](sd-plugins.md).
- **Manual moves** through the web File Manager or a script are your own
  responsibility: move `book.epub`, `book.jpg` and `book.opf` together.

The firmware's sidecar tables are in `lib/FsHelpers/SidecarFiles.h`. Cover
resolution, metadata resolution and the move, remove and stamp paths all read
them: `ReaderActivity::sidecarCoverPath` is a one-line delegate that adds only
its logging, and `MetadataSidecar::read()` finds the sidecar through
`SidecarFiles::metadataPath`. Two places still keep their own list, and need the
same change when a sidecar type is added: `Epub::ensureCoverImageCached` (the
cover extensions, when it copies a cover sidecar to `cover.img`) and
`LibraryBuilder`'s walk (`.opf` / `.OPF`, `isMetadataSidecar`).

This used to be three independent copies, which is exactly how `.opf` came to be
readable by the reader but left behind when a finished book moved. The copies had
also drifted: the move path derived its base name without checking for a path
separator, so a book with no extension inside a dotted folder (`/My.Books/untitled`)
took the dot from the folder.

Deleting a book on the device (the Library, Remove) removes its sidecars too
(`SidecarFiles::removeAll`). Deleting in All Files removes only the file
selected. Deleting through the web File Manager (`POST /delete`) or WebDAV
removes the book and its layout cache only; its sidecars stay behind as harmless
orphans, so delete `book.jpg` and `book.opf` yourself.

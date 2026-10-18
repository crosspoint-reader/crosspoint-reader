# File Formats

What the firmware keeps on the SD card: where each file lives, what writes it, and the version that
decides whether an old copy is still read. The sections after the overview give the layout of the
binary files worth reading on a card or when debugging a cache.

Binary files are little-endian unless noted. `String` is a `u32` byte length followed by the UTF-8
bytes (`serialization::writeString` in `lib/Serialization/Serialization.h`). `Storage.writeFile`
replaces a file whole (it writes `<path>.tmp` and renames it over the old one); `openFileForWrite`
truncates the file in place.

## Overview

### `/.crosspoint/`

| File | Written by | Version | Contents |
|---|---|---|---|
| `settings.json` | `JsonSettingsIO::saveSettings` | `fontSizeOrderV`: `CrossPointSettings::FONT_SIZE_ORDER_VERSION` (2); `gestureDefaultsV`: `GESTURE_DEFAULTS_VERSION` (3) | The settings. From `fontSizeOrderV` 2, `fontSize` and `txtFontSize` are point sizes; an older file is converted as it loads and saved again |
| `state.json` | `JsonSettingsIO::saveState` | none | The open book, the sleep image, a pending KOReader sync or bookmark jump, the Library tab last left (`libraryTab`) and the views of its lists (`recentBooksView`, `addedBooksView`, `authorsView`; an old `recentBooksGridView` is read to migrate) |
| `recent.json` | `JsonSettingsIO::saveRecentBooks` | `fontSizeOrderV` (2), stamped separately from `settings.json` | Recently opened books and their per-book overrides; `fontSizeOverride` is a point size from version 2 |
| `wifi.json`, `opds.json`, `koreader.json` | `JsonSettingsIO::saveWifi`, `saveOpds`, `saveKOReader` | none | Credentials, obfuscated. The web file endpoints refuse them (`lib/FsHelpers/ProtectedPaths.h`) |
| `weather_settings.json`, `weather_cache.json` | `WeatherSettingsStore`, `WeatherClient::saveCache` | none | Weather settings, and the last forecast (dropped when its `requestSignature` no longer matches the settings) |
| `autosync.json` | `AutoSyncState::saveToFile` | none | KOReader auto-sync state |
| `language.bin` | `I18n::saveSettings` | `SETTINGS_VERSION` (2) | `u8` version, then the UI language's code; version 1 (an enum index) is migrated. See [i18n.md](i18n.md) |
| `global_bookmarks.bin` | `GlobalBookmarkIndex::save` | `FILE_VERSION` (1) | Every book's bookmarks, updated from its `bookmarks.bin` as that saves |
| `reading-stats.bin` | `ReadingStatsStore` | `ReadingStatsSlotFile::kVersion` (1) | Reading history in fixed slots, magic `RSTB`: [design/reading-stats-slot-file.md](design/reading-stats-slot-file.md). A `reading-stats.json` from older firmware is imported once and renamed `reading-stats.json.imported` |
| `bootdiag.bin` | `BootDiagnostics` | `kRingVersion` (2) in `src/BootDiagRing.h` | A ring of the last 16 sleep and wake events, magic `WBD1` |
| `sleep_frame.bin` | `saveSleepFrameBuffer` (`src/main.cpp`) | none | The framebuffer kept for Quick Resume; deleted once read |
| `content-changed` | `HalStorage` | none | Empty marker: [below](#content-changed) |
| `library/library.bin` | `LibraryPublish` | `library::VERSION` (3) | The Library's book index: [below](#librarybin) |
| `fileindex/<hash>.idx` | `FileIndex` | `INDEX_VERSION` (2) | A large folder's sorted listing, magic `CPFI`; `<hash>` is FNV-1a 64 of the folder's path |
| `fonts/<Family>/<Family>_<size>.cpfont` | Font Manager, web upload | `CPFONT_VERSION` (4) in `lib/EpdFont/SdCardFont.cpp` | SD fonts; one of another version is refused. Downloads are staged in `<Family>__staging/` and swapped in. See [contributing/font-cache-structures.md](contributing/font-cache-structures.md) |
| `fontprev/*.fpv` | `FontPreviewCache::store` | `FORMAT_VERSION` (2) | Rendered font previews, magic `CPFP` |
| `opds_<hash>_disc.json` | `saveOpdsDiscovery` | `"v": 1` | An OPDS server's navigation entries and search template; `<hash>` is FNV-1a of the server's URL |
| `epub_<hash>/`, `xtc_<hash>/`, `txt_<hash>/` | | | One per book: next table |

### Per book

`<hash>` is `std::hash` of the book's path, so a book moved or renamed gets a new directory (the
`Epub`, `Xtc` and `Txt` constructors; `ReaderActivity::bookCacheDir`). TXT and Markdown books share
`txt_`. Settings → System → Clear Reading Cache removes these directories whole, progress and
bookmarks included, and `fontprev/` (`ClearCacheActivity`).

| File | Formats | Version | Contents |
|---|---|---|---|
| `progress.bin` | all | none (told apart by length) | Where the book was left: [below](#progressbin) |
| `bookmarks.bin` | all | `BookmarkStore::FILE_VERSION` (2) | `u8` version, `u16` count, then per bookmark `u16` spine, `u16` page, `u16` name length, the name. Version 1 (no names) still reads |
| `details.bin` | all | `BookDetailsCache` `VERSION` (3) | What the Library's lists show: [below](#detailsbin) |
| `thumb_*.bmp` | all | in the name | Cover thumbnails: [below](#cover-thumbnails) |
| `cover.bmp` | all | none | The sleep screen's cover (`cover_crop.bmp` for an EPUB's cropped one) |
| `book.bin` | EPUB | `BOOK_CACHE_VERSION` (11) | Metadata, spine and TOC: [below](#bookbin) |
| `fingerprint.bin`, `toc.retry` | EPUB | none | [below](#fingerprintbin-and-tocretry) |
| `pagelist.bin` | EPUB | none | The printed-page list: [below](#pagelistbin) |
| `css_rules.cache`, `css_bg.bin`, `css_fonts.bin` | EPUB | `CSS_CACHE_VERSION` (29), `BACKGROUND_IMAGES_FILE_VERSION` (1) | [below](#css_rulescache-css_bgbin-and-css_fontsbin) |
| `spines/<n>/<spine>_<hash>.bin` | EPUB | `SECTION_FILE_VERSION` | Laid-out pages: [`section.bin`](#sectionbin) |
| `spines/<n>/html_<spine>.bin` | EPUB | none | The spine item's inflated XHTML, banked by its build for the footnote pass and later builds; checked against the inflated size |
| `pages.bin` | EPUB | `kFormatVersion` (1) in `SpinePageIndex.cpp`, plus the section version and property hash | Each spine item's page count and size under one render variant (`SpinePageIndex`), to count pages across a chapter split over several files |
| `footnotes.bin` | EPUB | `CACHE_VERSION` (3) | Inline footnote previews: [below](#footnotesbin) |
| `images.bin`, `images.pending` | EPUB | `EpubImageManifest::VERSION` (5) | Image sizes, 12 B per image (FNV-1a 64 of its path in the EPUB, width, height), and the queue of images whose size is still to be read |
| `img_<hash><ext>`, `<image>.1bit.pxc`, `<image>.bayer.pxc` | EPUB | `PixelCache::PXC_MAGIC` | Images extracted from the book (`<hash>` is FNV-1a 64 of the entry), and their dithered pixel caches: `u16` magic, `u16` width, `u16` height, 2-bit pixels |
| `cover.img` | EPUB | none | The cover image's bytes, extracted once (or copied from a cover sidecar) |
| `linkstack.bin` | EPUB | `EpubLinkBackStack::kVersion` (1) | The link Back stack: [below](#linkstackbin) |
| `fonts.bin`, `fonts/<style>.ttf`, `fonts/accent/<style>.ttf` | EPUB | `BookFonts::kVersion` (4) | The book's embedded body and accent fonts (Publisher Fonts): [below](#fontsbin) |
| `kosync_push.bin` | EPUB | `LastPush::kVersion` (1) | Our last KOReader push: [below](#kosync_pushbin) |
| `koreader_docid.txt`, `koreader_syncmethod.txt` | EPUB | none | The KOReader document id, after the file size and fingerprint it was computed for, and the match method learned for the book |
| `index.bin` | TXT, Markdown | `CACHE_VERSION`: TXT 2 (magic `TXTI`), Markdown 4 (`MKDI`) | Page offsets under the current layout. `SleepActivity` checks the TXT magic and version too |
| `page_<n>.bw1`, `page_<n>.gr2` | XTC | none | Up to four pages' planes, transposed for drawing (`Xtc::kPageCacheMax`) |

Builds also write scratch files that they remove when done: `spine.bin.tmp` and `toc.bin.tmp`
(`BookMetadataCache`), `.items.bin` (`ContentOpfParser`), `.tmp.css` and `css_rules.compile.tmp` (the
CSS pass), `spines/<n>/anchors_<spine>.tmp` (a section's anchor map), `footnotes.bin.idx`,
`images.pending.tmp`, and an XTC's `*.t2bit` and `page_preview.*`.

### Elsewhere on the card

| File | Written by | Contents |
|---|---|---|
| `/fonts_families.bin` | Font Manager | The family list, parked during a download: [below](#fonts_familiesbin) |
| `/fonts_manifest.tmp`, `/fonts_manifest_web.tmp` | Font Manager, web UI | The downloaded font manifest; removed once parsed |
| `/.tmp_opds_cache.dat`, `/.tmp_opds_cover.*` | `OpdsBookBrowserActivity` | The OPDS browser's entries and the cover being shown; removed when it closes |
| `/sleep.bmp` | `BmpViewerActivity` | An image set as the sleep screen. A rendered one goes through `/sleep.bmp.tmp`, the old file kept as `/sleep.bmp.bak` until the swap is done |
| `/crash_report.txt` | `HalSystem` | The last panic |
| `/screenshots/*.bmp` | `ScreenshotUtil` | Screenshots |
| `<dictionary>.qidx`, `.sidx` | `Dictionary` | Lookup indexes beside a dictionary's `.idx` and `.syn`, `SIDECAR_VERSION` (2): [dictionary.md](dictionary.md) |

### In flash: the font area

Not on the card: the `spiffs` partition below the language slot holds one of two things, whichever the reader's
fonts need.

| Magic | Written by | Version | Contents |
|---|---|---|---|
| `CPFC` | `FlashFontPartition` | none | A `.cpfont` family copied from the card, mapped in place. See [contributing/font-cache-structures.md](contributing/font-cache-structures.md) |
| `CPTT` | `FlashGlyphStore` | `glyphstore::kFormatVersion` (2) | The TTF glyph store: [below](#the-ttf-glyph-store-cptt) |

## `book.bin`

Written by `BookMetadataCache`. The version byte is `BOOK_CACHE_VERSION` in `lib/Epub/Epub/BookMetadataCache.cpp`; it is bumped when the layout changes, and an older cache is rebuilt on the next open. The pattern below matches version 11.

ImHex Pattern:

```c++
import std.mem;
import std.string;
import std.core;

// === Configuration ===
#define EXPECTED_VERSION 11
#define MAX_STRING_LENGTH 65535

// === String Structure ===

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

// === Metadata Structure ===

struct Metadata {
    String title [[comment("Book title")]];
    String author [[comment("Book author")]];
    String language [[comment("BCP47 language tag")]];
    String coverItemHref [[comment("Path to cover image")]];
    String textReferenceHref [[comment("Path to guided first text reference")]];
    String series [[comment("Series name")]];
    String seriesIndex [[comment("Series index/position")]];
    String description [[comment("Book description / blurb")]];
} [[comment("Book metadata information")]];

// === Spine Entry Structure ===

struct SpineEntry {
    String href [[comment("Resource path")]];
    u32 cumulativeSize [[comment("Cumulative size in bytes"), color("FF6B6B")]];
    s16 tocIndex [[comment("Index into TOC (-1 if none)"), color("4ECDC4")]];
} [[comment("Spine entry defining reading order")]];

// === TOC Entry Structure ===

struct TocEntry {
    String title [[comment("Chapter/section title")]];
    String href [[comment("Resource path")]];
    String anchor [[comment("Fragment identifier")]];
    u8 level [[comment("Nesting level (0-255)"), color("95E1D3")]];
    s16 spineIndex [[comment("Index into spine (-1 if none)"), color("F38181")]];
} [[comment("Table of contents entry")]];

// === Book Bin Structure ===

struct BookBin {
    // Header
    u8 version [[comment("Format version"), color("FFD93D")]];
    
    // Version validation
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }
    
    u32 lutOffset [[comment("Offset to lookup tables"), color("6BCB77")]];
    u16 spineCount [[comment("Number of spine entries"), color("4D96FF")]];
    u16 tocCount [[comment("Number of TOC entries"), color("FF6B9D")]];
    u8 tocReliable [[comment("1 if TOC has >=25% spine coverage, 0 otherwise"), color("F4A261")]];

    // Metadata section
    Metadata metadata [[comment("Book metadata")]];
    
    // Validate LUT offset alignment
    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }
    
    // Lookup Tables
    u32 spineLut[spineCount] [[comment("Spine entry offsets"), color("4D96FF")]];
    u32 tocLut[tocCount] [[comment("TOC entry offsets"), color("FF6B9D")]];
    
    // Data Entries
    SpineEntry spines[spineCount] [[comment("Spine entries (reading order)")]];
    TocEntry toc[tocCount] [[comment("Table of contents entries")]];
};

// === File Parsing ===

BookBin book @ 0x00;

// Validate we've consumed the entire file
u32 fileSize = std::mem::size();
u32 parsedSize = $;

if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## `section.bin`

One file per spine item and per set of render settings: `spines/<spine / 32>/<spine>_<hash>.bin` in the book's cache directory, `<hash>` being the render settings' property hash in eight hex digits (`Section::getSectionFilePath`, `Epub::spineCacheDir`; 32 is `Epub::SPINE_CACHE_BUCKET_SIZE`). Up to five variants per spine item are kept; past that the oldest by modification date go (`Section::evictOldVariants`). Written and read by `lib/Epub/Epub/Section.cpp`. `SECTION_FILE_VERSION` at the top of that file is bumped on every layout change, so read it there; a number quoted here would go stale within weeks. A file with another version is rejected and rebuilt.

The property hash (`Section::calculatePropertyHash`) covers the header's render settings, font-size normalisation, the inline-footnote-preview setting and, when previews are on, `INLINE_FOOTNOTE_PREVIEW_LAYOUT_VERSION` (3): a change in how previews expand rebuilds only the sections that show them.

The file is, in order:

```text
header                  fixed size, offsets patched in when the build ends
pages                   page after page, each serialized by Page::serialize (lib/Epub/Epub/Page.h)
page LUT                u32 file offset per page
anchor map              u16 count, then (String id, u16 pageNumber) per entry
page break label map    u16 count, then (u16 pageIndex, String label) per entry
paragraph LUT           u16 count, then one 8-byte entry per page
```

`String` is a `u32` byte length followed by the UTF-8 bytes. All integers are little-endian.

### Header

These fields are the `header::*` constants in `Section.cpp`, in file order. The `static_assert` in `Section::writeSectionFileHeader` ties their sum to `header::kSize`.

| Field                  | Type    | Meaning                                                                 |
| ---------------------- | ------- | ----------------------------------------------------------------------- |
| `version`              | u8      | `SECTION_FILE_VERSION`                                                  |
| `fontId`               | s32     | Render settings: they also feed the property hash                       |
| `lineCompression`      | float   |                                                                         |
| `extraParagraphSpacing`| bool    |                                                                         |
| `paragraphAlignment`   | u8      |                                                                         |
| `viewportWidth`        | u16     |                                                                         |
| `viewportHeight`       | u16     |                                                                         |
| `hyphenationEnabled`   | bool    |                                                                         |
| `embeddedStyle`        | bool    |                                                                         |
| `bionicReadingEnabled` | bool    |                                                                         |
| `imageRendering`       | u8      |                                                                         |
| status                 | u8      | Bit flags: parse complete, and the "degraded" flags for a build that ran short of heap (image, table row, CSS) or hit a fixed capacity (simplified). The `kStatus*` constants in `Section.cpp` name them. Written as a bool by early builds, so a flag-less file still reads correctly |
| `pageCount`            | u16     |                                                                         |
| page LUT offset        | u32     |                                                                         |
| anchor map offset      | u32     |                                                                         |
| page break map offset  | u32     |                                                                         |
| paragraph LUT offset   | u32     |                                                                         |

### Paragraph LUT entry

One entry per page: `u32 visibleTextOffset + u16 paragraphIndex + u16 listItemIndex`.

- `visibleTextOffset` is the number of visible bytes (`lib/Epub/Epub/VisibleText.h`) of the chapter's source text before the page's first element. KOReader sync pushes it and resolves pulled positions to a page with it. Two consecutive pages may share one offset (an image or rule page followed by the text at that offset); a lookup answers the first of them.
- `paragraphIndex` is the 1-based `<p>` sibling index, matching KOReader's XPath `p[N]`.
- `listItemIndex` is the running `<li>` count, likewise for `li[N]`.

### Page break label map

Printed-page labels, from inline `doc-pagebreak` markers and from the per-book `pagelist.bin` below. See [epub-toc-navigation.md](epub-toc-navigation.md) for the source-format selection rules.

## `content.bin`

`content.bin` (magic `WBC1`) is the settings-independent product of the Stage-1 compile: parsed, CSS-resolved blocks keyed by the book's ZIP fingerprint. The layout is defined by the structs and the reader/writer in `lib/Epub/Epub/content/CompiledContent.h` and `CompiledContent.cpp`; read them for the fields. It is not part of the default section-cache path in this tree.

## `pagelist.bin`

Per-book cache file produced at index time from one of the EPUB printed-page sources (NCX `<pageList>`, EPUB 3 nav `<nav epub:type="page-list">`, or EPUB 2.01 `page-map.xml`). Consumed once per section build by `Section::createSectionFile`. Absent for books that have no printed-page data.

```text
u16 entryCount
struct PageListEntry {
    String href;    // normalised spine href, e.g. "OEBPS/c9_split_000.xhtml"
    String anchor;  // fragment id; empty means "start of file"
    String label;   // printed-page label, e.g. "42" or "iv"
}
PageListEntry entries[entryCount];
```

Selection rules (see `docs/epub-toc-navigation.md`):

- The EPUB 3 nav page-list parser runs first.
- The NCX `<pageList>` writer runs only if the nav writer produced nothing.
- The EPUB 2.01 `page-map.xml` writer runs only if `pagelist.bin` doesn't already exist on disk.
- Inline `doc-pagebreak` markers in XHTML are matched at chapter parse time and don't need the cache file; they coexist with whichever source above won.

## `fingerprint.bin` and `toc.retry`

Two small files beside `book.bin` (`lib/Epub/Epub.cpp`).

- `fingerprint.bin`: `u32` magic `XFP1`, then the `u64` ZIP content fingerprint the cache was built from. The cache directory is named after the book's path, so a different book copied over the same name shows up as a fingerprint mismatch, and the whole cache directory is removed. A cache without the file adopts the current fingerprint.
- `toc.retry`: one byte, the number of index builds that ended without the TOC the book declares (its nav or NCX did not parse, e.g. for want of heap). Written before `book.bin`. While it holds 1 or 2, the reader's open rebuilds the cache instead of reusing `book.bin`, counting the retry before it starts; at 3 (`MAX_TOC_LOSSES`) it stops trying. A build that gets the TOC removes the file, and a retry that recovers it also drops the section caches and `pages.bin`, whose pages were laid out without the TOC's anchors.

## `css_rules.cache`, `css_bg.bin` and `css_fonts.bin`

The book's compiled stylesheets, written by `CssParser` (`lib/Epub/Epub/css/`). `css_rules.cache` starts with a 14-byte header (`u8` version, `u16` rule count, `u32` selector candidates, `u32` unsupported-selector skips, `u8` flags, `u16` complex-table bytes), then the complex-rule table (compound, descendant and child selectors, which the hashed index cannot key; its layout is described at `CssParser::complexTable`), then the sorted `{hash, offset}` index, then the rule records (`u16` selector length, selector, `u16` source order, style payload). A selector declared again after other rules has a record per declaration, so rules of equal specificity can apply in stylesheet order. The version byte is `CssParser::CSS_CACHE_VERSION` (29); the comment above it lists what each version changed. A file with another version is deleted and the CSS compiled again, and the book's section caches go with it. It is compiled into `css_rules.compile.tmp` first.

`css_bg.bin` sits beside it and holds the CSS background pictures (`CssParser::backgroundImageFor`): the `no-repeat` `background-image` of each rule that sets one. Absent when the book has none.

```text
u8  version             BACKGROUND_IMAGES_FILE_VERSION (1)
u16 count               at most MAX_BACKGROUND_IMAGE_RULES (32) are read
count x {
    u16 selectorLen, selector bytes
    u16 pathLen, path bytes       the picture's path inside the EPUB, relative to its sheet's folder
}
```

`css_fonts.bin` holds the book's `@font-face` rules (`CssParser::loadFontFaces`, read one at a time by `CssParser::nextFontFace`), in source order, for its embedded fonts: at most `MAX_FONT_FACES` (32). Absent when the book declares none.

```text
u8  version             CSS_CACHE_VERSION (29)
then, to the end of the file, one record per face {
    u16 weight          font-weight, 100-900 (normal 400, bold 700)
    u8  italic          1 when font-style is not normal
    u8  nameLen, name bytes       the declared family name, unquoted; CssParser::fontFamilyHash of it is
                                  what a style's font-family is compared with
    u8  hrefLen, href bytes       the src url() chosen (TrueType, else OpenType, else another format;
                                  never WOFF2, SVG, EOT or data:), joined to its sheet's folder
}
```

`CssParser::deleteCache` removes all three files.

## `footnotes.bin`

The book's inline footnote previews (`lib/Epub/Epub/FootnotePreviews.*`), filled one spine item at a time by its section build. Little-endian:

```text
u32 magic               'FNP1'
u16 version             CACHE_VERSION in FootnotePreviews.cpp (3)
u16 count               at most FootnotePreviews::MAX_ENTRIES (2048)
u32 indexOffset
u8  resolved[64]        one bit per spine item whose links are scanned and whose notes are all stored
blob                    count x { u16 textLen, text }, at most 240 bytes of text each
index (at indexOffset)  count x { u32 keyHash, u32 blobOffset }, sorted by keyHash
```

The key is FNV-1a (32-bit) of `"<targetSpineIndex>#<fragment>"`. Each pass writes its notes over the old index and a merged index after them; while it runs, the old index is parked in `footnotes.bin.idx`. A store with another magic or version, or whose index does not end the file, is deleted and started again. Version 3 trims Unicode spaces at a note's edges and stores no note made of nothing else.

## `progress.bin`

Where the book was left, in its cache directory. The layout depends on the format.

**EPUB** (`EpubProgressRecord` in `src/activities/reader/EpubProgressRecord.h`):

```text
u16 spine
u16 page
u16 pageCount           0: unknown (written mid-build or by a sync restore)
u8  percent
u16 paragraph           the page's paragraph LUT entry; 0: none
u16 chapterPage         1-based page within the TOC chapter, as the status bar showed it
u16 chapterTotal        0: unknown
u8  printedPageLength   0 when the label is longer than 15 bytes
    printedPage         the status bar's printed-page label, e.g. "(42)"
```

Everything after `percent` is optional. The reader writes the first 7 bytes as pages turn, and the paragraph and the status-bar tail when it closes the book (`EpubReaderActivity::onExit`); the tail is for the sleep screen, which cannot lay the book out to count pages itself. `EpubProgressRecord::decode` accepts 4, 6, 7, 9 and 14 or more bytes: a 5-byte file is a torn write and is rejected, and a record torn inside the tail keeps what came before it. The progress badge in book lists reads only the percent byte (`BookProgressPresentation`).

**XTC**: `u32 page`, `u8 percent`. **TXT and Markdown**: `u16 page`, `u32` byte offset of the page, `u8 percent`. The TXT and Markdown readers take only the page, so the shorter files of older firmware, and the 6-byte one a bookmark jump writes, still read.

## `linkstack.bin`

The EPUB reader's Back stack of followed links (footnotes and others), beside `progress.bin`, so Back still returns to the link's origin after sleep or a reopen (`EpubLinkBackStack` in `src/activities/reader/EpubLinkBackStack.h`):

```text
u8 version              EpubLinkBackStack::kVersion (1)
u8 depth                1..3 (kMaxDepth); a full stack drops its oldest entry
depth x { u16 spine, u16 page, u16 pageCount, u16 paragraph }   oldest first; 0 = unknown / none
```

An empty stack is stored as no file. The file is written when the reader closes the book (and before a heap-recovery restart), and removed as soon as the stack changes while a copy is on the card, to be written again on the next close. A file of another version, of a size that does not match its depth, or naming a spine item the book no longer has is ignored and deleted. An entry is restored like a saved position: by its paragraph, else by its page rescaled to the page count, so it survives a layout change.

## `fonts.bin`

What `BookFonts::prepare` (`lib/Epub/Epub/BookFonts.*`) found when the Publisher Fonts setting first met the book: the family its body text is set in, whether it embeds it, and the accent family (the embedded family the most other blocks are set in), with where the copies are. Written once per book, last, after the font files; the next open trusts it.

```text
u8  version             BookFonts::kVersion (4)
u8  code                0 none: the book's body keeps the reader's font
                        1 family: its body family is in fonts/
                        2 + n: n failed attempts (a read failed); examined again at the next open, given up at 3
u32 body                the body text's font-family, as CssStyle::fontFamily keeps it (CssFontFamily: the family's
                        identity, its generic serif or sans-serif in the low two bits); 0 for the reader's font
u32 accent              the accent family's identity when it is in fonts/accent/, else 0
    names               the body family's name as the book declares it (empty unless code is 1), then, when there
                        is an accent family, '\0' and its name; to the end of the file
```

The body family's files sit in `fonts/`, the accent family's in `fonts/accent/`, one per style, named `<style>.ttf` (0 regular, 1 bold, 2 italic, 3 bold italic): plain TrueType, any IDPF or Adobe font obfuscation undone. The reader loads either directory as it loads a `.ttf` family directory on the card. A file of another version, or none, means the book has not been examined; `prepare` then removes the whole `fonts/` directory first.

## `kosync_push.bin`

Our own last successful KOReader push of the book (`LastPush` in `lib/KOReaderSync/LastPushCache.h`, written by `KOReaderSyncActivity::saveOwnLastPush`). A server record with the same document id and XPath is this push, so the sync screen takes its position from here instead of mapping the XPath back through the chapter.

```text
u8  version             LastPush::kVersion (1)
    documentId          32 ASCII hex characters, no terminator
u8  xpathLen            1..191
    xpath               no terminator
s32 spine
s32 page
u32 visibleOffset       the page's visible-text offset (see the paragraph LUT above)
u8  hasVisibleOffset
```

The file is exactly `47 + xpathLen` bytes; anything else reads as no cached push. Only a push that carried the page's visible-text offset is recorded; any other push removes the file, so it always holds the last push or nothing.

## `details.bin`

What the Library's book lists show for a book (`BookDetailsCache` in `src/activities/home/BookDetailsCache.cpp`, `BookDetails.h`), in the book's cache directory whatever its format:

```text
u8  version             BookDetailsCache VERSION (3)
u32 bookSize            the book's size when recorded
u32 sidecarStamp        SidecarFiles::metadataStamp of its .opf sidecar; 0: none
String title
String author           every author credited, for display
String series
String seriesIndex
String primaryAuthor    the first creator credited as author: what the Authors tab groups by
String authorSort       its opf:file-as; empty when the book gives none
String isbn             the first dc:identifier that is an ISBN, without its prefix; may be empty
String asin             the first that is an Amazon ASIN; may be empty
```

A file of another version, or recorded for another book size or sidecar stamp, is ignored and the book read again (`BookDetailsLookup::parse`): an EPUB's OPF (never `book.bin`, which keeps no primary author), an XTC's header, and for every format the `.opf` sidecar over it. An EPUB whose parse failed is not recorded. Version 2 added `primaryAuthor` and `authorSort`; version 3 `isbn` and `asin`, which KOReader sync sends with **Send Document Metadata** (`KOReaderSyncMetadata::forBook`).

## Cover thumbnails

1-bit BMPs in the book's cache directory, made from the cover sidecar or the embedded cover (`ReaderActivity::ensureCoverThumb`, `CoverThumbLoader`):

- `thumb_<W>x<H>.bmp`: the cover drawn into a `W` x `H` box. The Library's Covers view and the finished-book screen fit the cover inside the box, whole (`CoverGridLayout::kThumbCrop` false); the Home carousel crops it to fill its own boxes. The grid's box is the size its cells come out at, at most 196 x 240 (`CoverGridLayout::kThumbWidth`, `kThumbHeight`), which the X3 and X4 use; where the cells come out smaller the box does too (e.g. 154 x 214 on the T5S3; `CoverGridLayout::compute`). The name carries the box, so a new box makes new files.
- `thumb_<H>.bmp`: the single-height form the other Home themes draw, `H` x 0.6 wide.

A book with no cover at all gets a 1 x 1 placeholder BMP at each size (`ReaderActivity::writeCoverPlaceholderBmp`), so no screen tries it again, and the theme draws its no-cover tile. `recent.json` stores a book's cover as `<cache dir>/thumb_[HEIGHT].bmp` (`ReaderActivity::coverThumbPlaceholder`); `UITheme::getCoverThumbPath` puts in `<H>` or `<W>x<H>` when drawing.

## `library.bin`

`/.crosspoint/library/library.bin`: the book index the Library's New and Authors tabs read, built by
`LibraryBuilder` and read by `LibraryIndexReader` (`lib/LibraryIndex/`). The structs are in
`LibraryFormat.h`; the design is in [design/library-index.md](design/library-index.md). Written under a
temporary name and renamed into place. Little-endian, packed. A wrong magic or version, or sections
that do not fit the file, make it invalid, and a build replaces it.

| Section | Layout |
|---|---|
| Header, 48 B | magic `WLIB`; `u8` version (3); `u8` flags (bit 0: partial, the card held more than 2,000 books); `u8` acceptRules (bit 0: the *Show Hidden Files* setting the walk used; bits 1-7: the revision of the folders the walk always leaves out, now 1); `u8` reserved; `u32` buildGen; `u16` bookCount, authorCount, newCount; `u16` reserved; `u32` offsets of the five sections below; `u32` blob length; `u32` newestDate, the newest date among the books and the folders the walk listed (0 for none) |
| Records | bookCount × 24 B, **in identity order**: `u32` identity, authorHash, date (FAT `date << 16 \| time`), pathOff, sidecarSig, firstSeen |
| New | newCount (≤ 10) × `u16` record index, newest first |
| Authors | authorCount × 12 B, **in sort-key order**: `u32` hash, nameOff; `u16` firstBook, count |
| Author books | bookCount × `u16` record index; an author's books are the slots `[firstBook, firstBook + count)` |
| Blob | Strings, each a `u16` length then that many bytes: book paths; for each author its name as the books spell it, then its filing name ("Pratchett, Terry") |

`pathOff` and `nameOff` count from the start of the blob. A book's identity is FNV-1a of its file
name, lower-cased, and its size (`LibraryKeys::bookIdentity`), so a book moved to another folder keeps
its record and its `firstSeen`. Its sidecar signature is FNV-1a of its `.opf` sidecar's size and date;
when it changes, the next build looks the author up again.

Reserved author hashes: `0`, no author ("Unknown author"); `0xFFFFFFFF`, not known yet ("Not yet
indexed"). Reserved sidecar signatures: `0`, no sidecar; `0xFFFFFFFF`, a sidecar the walk could not
pair with its book (its folder held more than `LibraryBuilder::MAX_SIDECARS` sidecars). Version 1 stored
an author's folded sort key where version 2 stores its filing name; version 3 adds `newestDate`.

A build publishes the index as soon as the walk is joined, with the authors it does not know yet as
`0xFFFFFFFF`, then looks them up one book per step and publishes again every 100 (`Config::republishEvery`)
and at the end. Its working files sit beside the index and are removed when the build finishes:
`stage.bin` (16 B per book: identity, date, sidecarSig, pathOff), `paths.bin` (the paths),
`records.bin` (the joined records) and `names.bin` (per author: `u32` hash, `u8` flags with bit 0 set
when the filing name came from a file-as, then the name and filing name as blob strings). A publish
also writes `library.bin.names`, the author strings, and removes it when done.

## `content-changed`

`/.crosspoint/content-changed`: an empty marker file that says the card changed since the last
finished library build, across reboots. `HalStorage::noteContentChange` leaves it on the first change
after a build: anything the firmware creates, removes, renames or opens for writing outside
`/.crosspoint`, the start and end of a USB Drive session, and a book or folder the Books tab lists with
a date later than `library.bin`'s `newestDate` (`LibraryFreshness::checkListedEntry`,
`HalStorage::noteFoundChange`). `HalStorage::markContentSeen` removes it when a build finishes, unless
another change came while the build ran. While it is there, the next visit to New or Authors rebuilds
the index (`LibraryStaleness::rebuildNeeded`). Being inside `/.crosspoint`, writing it is not itself a
change.

## `fonts_families.bin`

`/fonts_families.bin`, at the card's root: the Font Manager's list of downloadable families, parked on
the card during a download so its heap block can go to the TLS session
(`FontDownloadActivity::stashFamiliesToSd`, `restoreFamiliesFromSd`). The file is the `FontCatalog`
block itself (`src/util/FontCatalog.h`), written in one write and read back in one read, in the
device's byte order:

| Part | Layout |
|---|---|
| Header, 16 B | `u32` magic `CPFC`; `u16` version (1); `u16` families, files, stringBytes; `u32` blockBytes, the whole block |
| Families | 12 B each: `u32` totalSize; `u16` name, description, firstFile; `u8` flags (bit 0 installed, bit 1 update available, bit 2 resumable download); `u8` 0 |
| Files | 12 B each: `u32` size, crc32; `u16` name; `u8` hasCrc32; `u8` 0 |
| Strings | NUL-terminated; `name` and `description` are offsets into them |

A family's files run from its `firstFile` to the next family's. `FontCatalog::readFrom` rejects a file
of another magic, version or size, or one that does not hold together (an offset past the strings,
files out of order, a total that does not add up). The file is left in place after a restore; the next
stash overwrites it. The manifest the list is built from is
downloaded to `/fonts_manifest.tmp` (`/fonts_manifest_web.tmp` from the web UI) and removed once
parsed.

## The TTF glyph store (`CPTT`)

`FlashGlyphStore` (`lib/EpdFont/ttf`) keeps the reader's TrueType families in the font area: their files, and
every glyph drawn from them, so a glyph is rendered once and then read from flash. It holds a family set: one
family, or several drawn together (the body's, and a book's accent family under Publisher Fonts), installed
together. The layout is
`glyphstore::Layout` (`GlyphStoreFormat.h`); the design and its reasons are in the TTF spec, sections 4 and 11.1.
Every byte is programmed at most once between erases, so each marker below is one that only a complete write
produces; a power cut leaves at worst bytes nothing references.

| Offset | Size | Contents |
|---|---|---|
| 0 | 4 KB sector | `Header`, written last at install: `u32` magic `CPTT`, `u16` version, `u16` header bytes, the set's 16-byte identity, the `Geometry` (mode, style count, the regions' offsets and sizes), 8 `StyleInfo` (a style block's offset and size; in SD mode also its file and table map), then `u32` FNV-1a over all of it |
| 4 KB | 4 KB sector | The state: 32 face entries `{u32 face, u32 claimed}`, then the `u32` wipe word |
| 64 KB | 64 KB pages | The style blocks, each family's styles in the set's order, each on its own page so one maps alone |
| `indexOffset` | whole pages | Glyph index: 8-byte slots `{u32 key, u32 value}`, open addressing, at most 128 probes |
| `pairOffset` | 64 KB | Kerning pairs: `{u32 left << 16 \| right, u32 pairStyle << 16 \| i16 font units}` |
| `logOffset` | whole pages | Glyph records, append only: `u32 key`, the 16-byte `EpdGlyph`, the 2-bit bitmap, padded to 4 bytes |

- **A face** is a family at one size: `face = family << 24 | ppem` (26.6 pixels per em; the family is its index in
  the set). An entry is in use once `claimed` reads 0. A glyph key is `codepoint | style << 21 | faceIndex << 23`,
  bits 28-31 zero, so a key never reads as erased. The 32 entries are every family's; taking the last one, or asking
  for a 33rd, makes the wipe pending.
- **A slot's value** is the record's `dataOffset` (its log offset + 20), or 0: the font has no glyph for that key.
- **A pair's style** is `family << 2 | style`; the value is the kerning in font units, the same at every size.
- **The wipe word** reads erased unless a wipe is pending. A wipe erases the index, the pairs and the log (only
  sectors that hold data), and the state sector last, at the next book open.
- **The identity** is FNV-1a over the families' identities in the set's order (`ttf::setIdentity`); a family's is
  FNV-1a over each style file's name, size and modification time and the engine's render version
  (`ttf::familyIdentity`). A store of another identity, or one whose header fails its check, is installed again.

Version 1 held one family: four style blocks, and a size table of bare ppems with pairs keyed by style alone. A
version-1 header reads as not installed, so the first load installs the set over it.

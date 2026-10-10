# File Formats

These formats describe the SD-card cache files under `/.crosspoint/epub_<hash>/`.
All POD fields are written in the ESP32 little-endian representation used by
`Serialization.h`; strings are length-prefixed UTF-8.

## `book.bin`

### Version 10

`book.bin` stores EPUB metadata plus lookup tables for spine and TOC entries.
The current firmware writes this version from `BookMetadataCache`.

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 10
#define MAX_STRING_LENGTH 65535

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

struct Metadata {
    String title [[comment("Book title")]];
    String author [[comment("Book author")]];
    String language [[comment("Book language code")]];
    String coverItemHref [[comment("Path to cover image")]];
    String textReferenceHref [[comment("Path to guided first text reference")]];
};

struct SpineEntry {
    String href [[comment("Resource path")]];
    u32 cumulativeSize [[comment("Cumulative uncompressed spine size through this entry")]];
    s16 tocIndex [[comment("Index into TOC, or inherited/previous TOC index when no direct entry exists")]];
};

struct TocEntry {
    String title [[comment("Chapter/section title")]];
    String href [[comment("Resource path")]];
    String anchor [[comment("Fragment identifier")]];
    u8 level [[comment("Nesting level")]];
    s16 spineIndex [[comment("Index into spine (-1 if none)")]];
};

struct BookBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    u32 lutOffset [[comment("Offset to lookup tables")]];
    u16 spineCount;
    u16 tocCount;

    Metadata metadata;

    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }

    u32 spineLut[spineCount] [[comment("Spine entry offsets")]];
    u32 tocLut[tocCount] [[comment("TOC entry offsets")]];

    SpineEntry spines[spineCount];
    TocEntry toc[tocCount];
};

BookBin book @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## `section.bin`

### Version 55

Each TextBlock adds a uint16 `paragraphStartWord` after `textBytes`. It is the
visual index of the paragraph's first logical word, or `UINT16_MAX` for a
continuation line. Clipping uses this marker independently of source-offset
gaps. Older completed and partial section caches rebuild automatically;
book metadata and reading progress are kept.

### Version 54

The serialized layout is unchanged. Word source ranges and split offsets now
include codepoints absorbed by NFC composition. The high bit of each word's
style byte marks a discretionary hyphen, so clipping can remove it independently
of source length. Rebuild completed and partial section caches to correct
clipping spaces and anchors for decomposed text.

### Version 53

Each TextBlock arena starts with one 8-byte source range per word (two uint32
chapter-visible Unicode-codepoint offsets, start inclusive and end exclusive).
Ranges follow words through BiDi ordering and line wrapping. This version also
includes the version 52 redaction layout changes. Older completed and partial
section caches are rebuilt automatically; book and progress files are kept.

### Version 52

The serialized layout is unchanged. Missing full-block (`U+2588`) and black-square
(`U+25A0`) symbols now use font-sized solid rectangles instead of replacement
glyphs. Rebuild older sections so cached line breaks and word positions match
their new widths.

### Version 50

The header adds `paragraphIndentSpaces` after `extraParagraphSpacing`. The value
participates in cache validation, so sections with different indentation settings
are rebuilt. Version 49 was used by pre-release builds with a different header
layout and is skipped to prevent reuse of those caches.

### Version 48

Version 48 keeps the version 47 serialized layout unchanged. It was bumped
because Hangul text no longer has implicit line-break opportunities between
syllables: Korean words wrap at spaces (like CSS `word-break: keep-all`), and
with hyphenation enabled a word may also split at the end of a line wherever the
CJK line-breaking rules allow, without an inserted hyphen. Justification stretches only word spaces. Cached line breaks and word
positions from version 47 no longer match.

### Version 47

The section header adds signed `characterSpacing` (pixels) and unsigned
`wordSpacingPercent` after `focusReadingEnabled`; both participate in cache
validation. Each TextBlock's BlockStyle stores only `characterSpacing` after
`directionDefined`. Word spacing is resolved into cached word positions during
layout. Sections from earlier versions are rebuilt.

### Version 46

Version 46 keeps the version 45 serialized layout unchanged. It was bumped
because ordered lists now number their items, `list-style-type: none`
suppresses list markers, and `<ul>`/`<ol>` containers contribute their own
margins and padding to child block insets, changing cached word contents and
page layout.

### Version 45

Version 45 keeps the version 44 serialized layout unchanged. It was bumped
because internal EPUB links now preserve CSS superscript and subscript styles,
changing their cached word-style flags and page layout.

### Version 44

Each file in `sections/*.bin` stores one laid-out spine section. The header is
also the cache-busting key: if any layout-affecting setting differs from the
current reader settings, the section is discarded and rebuilt.

Version 44 appends the internal-link rectangles produced during text layout to
each serialized page. The reader uses these rectangles for touch navigation;
older caches are rebuilt because they contain no link geometry.

Version 43 keeps the version 42 serialized layout unchanged. It was bumped
because paragraph base direction now excludes direction changes from inline
elements.

Version 42 keeps the version 41 serialized layout unchanged. It was bumped
because closing a block now strips inherited vertical margins and padding.

Version 41 keeps the version 40 serialized layout unchanged. It was bumped
because simple HTML table rows are now laid out as positioned columns rather
than flattened paragraphs with synthetic row/cell labels.

Version 40 keeps the version 39 serialized layout unchanged. It was bumped
because ruby groups now remain intact when large text blocks are soft-flushed.

Version 39 keeps the version 38 serialized layout unchanged. It was bumped
because image top margins are now clamped to keep full-height images within the
page viewport.

Version 38 keeps the version 37 serialized layout unchanged. It was bumped
because Focus Reading now permits line breaks at visible hyphens and dashes
and hyphenates focus-split words as a whole, changing cached page layout.

Version 37 increases the fixed-size footnote href field from 96 to 256 bytes.
This changes each serialized footnote record from 128 to 288 bytes, so older
section caches must be discarded and rebuilt.

Version 36 keeps the version 35 serialized layout unchanged. It was bumped
because ruby and justified text positioning and CJK line breaking now use
corrected word measurements, so version 35 cached page layouts no longer match.

Version 35 adds a header offset and a `uint32_t` entry per page for the
visible-text offset LUT. The other section LUTs remain unchanged.

Version 34 is binary-identical to version 33. The version was bumped because
word-gap suppression was narrowed to tokens glued together in the source: v33
dropped the gap between any two words meeting at a CJK break opportunity, which
collapsed the spaces between Hangul words, so v33 word positions no longer match
what the layout engine now produces.

Version 30 is binary-identical to version 29. The version was bumped because
Arabic contextual shaping changed text measurement (`getTextAdvanceX` now
measures the shaped visual text), so word positions cached by v29 no longer
match what `drawText` renders.

Version 28 introduced serialized word style bits for underline, strikethrough,
superscript, and subscript. The format also includes:

- cache-busting fields for paragraph alignment, hyphenation, embedded CSS,
  image rendering mode, and Focus Reading
- page offset LUT
- per-page visible-text offset LUT (zero-based Unicode codepoints in `<body>`)
- anchor-to-page map for fragment and footnote navigation
- paragraph and list-item LUTs retained for navigation and legacy sync fallback
- optional per-word Focus Reading split metadata
- per-page footnote entries
- serialized word style bits for underline, strikethrough, superscript, and
  subscript
- flat TextBlock word storage (v29): per-word arrays plus one shared
  NUL-terminated text blob, replacing v28's length-prefixed word strings. The
  on-disk order mirrors the in-RAM arena so the firmware reads a whole block
  payload with a single allocation and a single SD read

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 50
#define MAX_STRING_LENGTH 65535
#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 256

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

enum PageElementTag : u8 {
    TAG_PageLine = 1,
    TAG_PageImage = 2,
    TAG_PageHorizontalRule = 3
};

enum WordStyle : u8 {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,
    STRIKETHROUGH = 8,
    SUP = 16,
    SUB = 32
};

enum TextAlign : u8 {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    NONE = 4
};

struct BlockStyle {
    TextAlign alignment;
    bool textAlignDefined;
    s16 marginTop;
    s16 marginBottom;
    s16 marginLeft;
    s16 marginRight;
    s16 paddingTop;
    s16 paddingBottom;
    s16 paddingLeft;
    s16 paddingRight;
    s16 textIndent;
    bool textIndentDefined;
    bool isRtl;
    bool directionDefined;
    s8 characterSpacing;
};

struct TextBlock {
    u16 wordCount;
    u8 hasFocus;
    u16 textBytes [[comment("Total size of text[], including one NUL per word")]];

    if (wordCount > 0) {
        u16 textOff[wordCount] [[comment("Byte offset of word i's text within text[]")]];
        s16 wordXPos[wordCount];
        if (hasFocus != 0) {
            u16 wordFocusSuffixX[wordCount] [[comment("Suffix x offset from word start")]];
        }
        WordStyle wordStyle[wordCount];
        if (hasFocus != 0) {
            u8 wordFocusBoundary[wordCount] [[comment("UTF-8 byte boundary between bold prefix and suffix")]];
        }
        char text[textBytes] [[comment("All words back to back, each NUL-terminated")]];
    }

    BlockStyle blockStyle;
};

struct ImageBlock {
    String imagePath;
    String srcPath;
    s16 width;
    s16 height;
};

struct PageLine {
    s16 xPos;
    s16 yPos;
    TextBlock block;
};

struct PageImage {
    s16 xPos;
    s16 yPos;
    ImageBlock image;
};

struct PageHorizontalRule {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 thickness;
};

struct PageElement {
    PageElementTag pageElementType;
    if (pageElementType == TAG_PageLine) {
        PageLine pageLine [[inline]];
    } else if (pageElementType == TAG_PageImage) {
        PageImage pageImage [[inline]];
    } else if (pageElementType == TAG_PageHorizontalRule) {
        PageHorizontalRule horizontalRule [[inline]];
    } else {
        std::error(std::format("Unknown page element type: {}", pageElementType));
    }
};

struct FootnoteEntry {
    char number[FOOTNOTE_NUMBER_LEN];
    char href[FOOTNOTE_HREF_LEN];
};

struct Page {
    u16 elementCount;
    PageElement elements[elementCount] [[inline]];

    u16 footnoteCount;
    FootnoteEntry footnotes[footnoteCount];
};

struct AnchorEntry {
    String anchor;
    u16 page;
};

struct AnchorMap {
    u16 count;
    AnchorEntry entries[count];
};

struct ParagraphLut {
    u16 count;
    u16 paragraphIndex[count];
};

struct SectionBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    s32 fontId;
    float lineCompression;
    bool extraParagraphSpacing;
    u8 paragraphIndentSpaces;
    u8 paragraphAlignment;
    u16 viewportWidth;
    u16 viewportHeight;
    bool hyphenationEnabled;
    bool embeddedStyle;
    u8 imageRendering;
    bool focusReadingEnabled;
    s8 characterSpacing;
    u8 wordSpacingPercent;

    u16 pageCount;
    u32 pageLutOffset;
    u32 anchorMapOffset;
    u32 paragraphLutOffset;
    u32 listItemLutOffset;
    u32 visibleTextLutOffset;

    Page pages[pageCount];

    u32 currentOffset = $;
    if (currentOffset != pageLutOffset) {
        std::warning(std::format("Page LUT offset mismatch: expected 0x{:X}, got 0x{:X}", pageLutOffset, currentOffset));
    }

    u32 pageLut[pageCount] [[comment("Page data offsets")]];

    if (anchorMapOffset != 0) {
        AnchorMap anchorMap @ anchorMapOffset;
    }

    if (paragraphLutOffset != 0) {
        ParagraphLut paragraphLut @ paragraphLutOffset;
    }

    if (listItemLutOffset != 0 && paragraphLutOffset != 0) {
        u16 listItemIndex[paragraphLut.count] @ listItemLutOffset;
    }

    if (visibleTextLutOffset != 0) {
	u32 visibleTextOffset[pageCount] @ visibleTextLutOffset;
    }
};

SectionBin section @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## CLX1 — library index (`.crosspoint/library.idx`)

Written by `lib/LibraryIndex/LibraryBuilder.cpp`, read by `LibraryIndexFile`. One
file describing every book on the card, so the shelf can sort and search
thousands of titles without opening any of them.

Format version 3. An index written by another version fails validation on open
and is rebuilt; that is the entire migration mechanism.

### Layout

| Section | Offset | Contents |
|---|---|---|
| Header | 0 | 64 bytes, `ClixHeader` |
| Folders | `folderStart` | length-prefixed paths, one per folder |
| Records | `recordStart` | `bookCount` × 128-byte `ClixRecord` |
| Permutations | `permStart` | Three arrays of `bookCount` u16 values: author, arrival, and group order |
| Groups | `groupStart` | `groupCount` × 64-byte `ClixGroupEntry` |
| Group refs | `groupRefStart` | `bookCount` × 4-byte `ClixGroupRef`, parallel to the records |
| Name blob | `nameStart` | per record: path hash, name, canonical author, title, source author, series position, the four group fields (see below) |

The arrival permutation runs oldest first, keyed by the record's FAT
modification time (when the file landed on the card); `firstSeen` — the
build-assigned discovery counter — breaks ties and carries books whose
filesystem reports no time. Fold version 3 introduced the timestamp key; a
fold bump rebuilds ranks while preserving `firstSeen`.
Fold version 4 preserves leading articles in title sort and search keys.

Sections are 512-byte aligned so each starts on an SD block boundary.

### Records are exactly 128 bytes

A fixed stride is what lets the reader seek straight to record *n* without an
offset table, and read a screenful in one 4 KB block. `static_assert` enforces it.

Each record carries `fold[96]`, the title normalised for search and sorting —
accents stripped, case dropped, leading articles preserved — and `authorKey[12]`,
the author's words folded and sorted so that "Victor Hugo" and "Hugo Victor" group as
one person. `authorKey` is a GROUPING key, not an ordering one: the shelf orders by
surname, derived separately from the display name.

The byte before the folded title records metadata extraction status: not
attempted, extracted, or failed. The final four bytes contain the packed FAT
modification date and time returned by SdFat. A zero timestamp is not trusted.
These fields occupy the alignment and reserved bytes from version 1, so the
record remains exactly 128 bytes.

The header records whether EPUB metadata extraction was enabled for the build.
This prevents a metadata-disabled rebuild from making filename fallbacks look
fresh to a later metadata-enabled build.

### Groups

`groupKind` selects the metadata field used for grouping. All kinds use the same
sections. Series stores a position within a group.

| Kind      | `groupKind` | OPF source                                  | Position     | Ungrouped heading |
|-----------|-------------|---------------------------------------------|--------------|-------------------|
| None      | 0           | none                                        | none         | none              |
| Series    | 1           | `calibre:series` or `belongs-to-collection` | series index | Standalone        |
| Publisher | 2           | first non-blank `dc:publisher`              | none         | Unknown publisher |
| Language  | 3           | first non-blank `dc:language`, normalised   | none         | Unknown language  |
| Subject   | 4           | first non-blank `dc:subject`                | none         | No subject        |

Calibre series tags take precedence over EPUB 3 collections. Otherwise, use
the first collection typed as `series`, or the first untyped collection if no
series is found. Ignore collections marked as another type. The parser stores
up to four collections and eight refinements per package document.

Language tags use a lowercase primary subtag and map ISO 639-2 codes to ISO
639-1 (`eng` to `en`, `fre` and `fra` to `fr`). The second subtag is kept only
when it matches a firmware translation (`pt-BR`, `pt-PT`, `ca-valencia`);
later subtags are discarded. For example, `en`, `EN`, `en-US` and `eng` all
use the stored tag `en`.

Placeholder codes `und`, `mul`, `zxx` and `mis` are treated as missing values.
The UI displays a native language name when available, otherwise the uppercase
tag. Stored tags are independent of the selected UI language.

#### Header fields

| Field           | Type | Meaning                                                              |
|-----------------|------|----------------------------------------------------------------------|
| `groupCount`    | u16  | Number of group table entries                                        |
| `groupStart`    | u32  | Group table offset                                                   |
| `groupRefStart` | u32  | Per-book group reference offset                                      |
| `groupedCount`  | u16  | Number of grouped books; start of the ungrouped block in group order |
| `groupKind`     | u8   | Configured `GroupKind`, retained if table allocation fails           |

#### Group entries and references

Group data is stored separately to keep book records at 128 bytes.
`ClixGroupEntry` has a fixed 64-byte stride: group *g* is at
`groupStart + 64g`, and eight entries fit in one sector. Its ID is its ordinal
in alphabetical group order.

```cpp
struct ClixGroupEntry {        // 64 bytes
  uint16_t bookCount;
  uint8_t  nameLen;            // <= 61
  char     name[61];           // heading, truncated at a UTF-8 boundary
};
```

The heading stores a byte length and has no terminating NUL. Identity and
sorting use the source value in the name blob, capped per field by the writer.
Distinct groups can have the same shortened heading so the UI detects group
boundaries by ID.

`ClixGroupRef` has a four-byte stride, parallel to the book records:

```cpp
struct ClixGroupRef {          // 4 bytes
  uint16_t groupId;            // CLIX_GROUP_NONE (0xFFFF) if ungrouped
  uint16_t position;           // hundredths; GROUP_POSITION_NONE (0xFFFF) if absent
};
```

Series positions use hundredths: 250 means 2.5. Zero is valid. Values above
`GROUP_POSITION_MAX` (0xFFFE, or 655.34) are clamped. Missing positions use
`GROUP_POSITION_NONE`, which sorts after numbered books. Other group kinds
always use `GROUP_POSITION_NONE` and sort books by title.

The group permutation lists grouped books first, groups A-Z by folded source
value, then books by position and title within each group. Ungrouped books
follow in title order. A source value whose fold is empty is treated as missing.
Descending order reverses the entire permutation.

#### Building and allocation failures

When grouping is enabled, each book's four fields and series position are
staged in a 512-byte record: two bytes for the position, four for lengths,
and 506 for field data. The record is exactly one sector, so each random read
during sorting touches one aligned block. Fields are capped at 180 bytes
(series), 120 (publisher), 16 (language) and 190 (subject) and truncated at a
UTF-8 boundary. A normalised language tag is at most 15 bytes, so its slack
goes to series and subject. Fields are packed in series, publisher, language,
subject order. Unused space in one field does not increase another field's
limit. With grouping off, no group staging file is created.

Sorting compares folded names in 12-byte chunks, reading more chunks when
prefixes match. The key array uses 16 bytes per book. Names are read into one
reusable scratch buffer and folded in place. Order and group ID arrays use
another four bytes per book until the index is written. Series also needs a
two-byte position per book for in-series ordinals. SD reads finish before each
in-memory sort. A staging read or seek failure stops the build and preserves the
previous index.

If group sorting cannot allocate its buffers or the core sort arrays are
unavailable, the builder writes zero group counts and sets `GROUPS_DEGRADED`.
It retains `groupKind` and the source fields for search and later rebuilds.
The group tab is hidden. Keeping the kind prevents repeated rebuilds on
Library entry; an explicit rebuild retries the table even if no books changed.
Failure to allocate required staging buffers stops the build.

#### Validation

The header is rejected unless:

- `groupKind` is a known value, and is None when metadata extraction is disabled.
- None has both group counts zero.
- `GROUPS_DEGRADED` has an enabled kind and both group counts zero.
- Both counts are at most `bookCount`.
- Zero `groupCount` has zero `groupedCount`.
- `groupCount` is at most `groupedCount`.
- Group section offsets match `layoutSections` for the stored counts.

Enabled kinds may have zero counts when no books have usable values.
These checks validate the header, not the contents of individual groups.
Out-of-range group IDs in book references are treated as ungrouped on read.

#### Kind changes and metadata reuse

Grouped builds retain all four source fields and the series position including
when table allocation fails. Changing between enabled kinds can therefore
reuse unchanged books' fields and sort by the new kind without opening those
books. If a book's stored fields are malformed or unreadable, only that book
is parsed again.

An index of kind None stores empty group fields. Enabling grouping after None
requires package document reads; disabling grouping can reuse unchanged titles
and authors. Metadata reuse still requires the freshness checks below.
When grouping is off, `book.bin` may supply title and author unless the book
changed, has no timestamp, or previously failed metadata extraction.

The Library rebuilds when the installed kind differs from the configured kind.
If replacement fails, the tab uses the previous index's kind. Disabling metadata
stores None. The group tab requires at least one group.

### The name blob

Per record, at `nameStart + nameOff`:

```text
[u64 pathHash]   FNV-1a fingerprint of the complete path
[nameLen bytes]  filename, without the directory
[u8][author]     display author, one spelling chosen per authorKey across the library
[u8][title]      the book's own title, or length 0 if it never gave one
[u8][source]     cleaned author spelling before the library-wide spelling vote
[u16 position]   series position in hundredths, or GROUP_POSITION_NONE
[u8][series]     source fields, capped per field by the writer
[u8][publisher]  zero length when absent; truncated at UTF-8 boundaries
[u8][language]   normalised language tag
[u8][subject]    all four empty, position NONE, when groupKind is None
```

The filename must stay the first textual field and stay the filename: `readPath`
rebuilds a book's path from it, so writing the display title there makes the book
impossible to open. That was a real defect, and it is why title has its own field.

The source author is separate from the displayed canonical author so a later
rebuild can repeat the spelling vote after books are added or removed. Existing
display reads still stop at the author or title fields and retain their offsets.

Blob readers accept source fields up to `CLIX_SOURCE_GROUP_BYTES` (255 bytes).
The writer's per-field caps mean source values that differ only after the cap
can share a group. Series position is stored here for metadata reuse;
`ClixGroupRef` also stores it when grouping by Series.

### Freshness and unchanged rebuilds

Reconciliation treats the persisted 64-bit complete-path fingerprint as the
book identity. Metadata is reused only when the fingerprint, size, nonzero FAT
timestamp, fold version, metadata mode, and expected extraction status agree.
Grouped metadata can be reused from any previous enabled kind. An unchanged
index is retained only if the group kind matches and `GROUPS_DEGRADED` is clear;
otherwise the builder writes a replacement using the reused fields.
EPUBs with a zero timestamp or a previous extraction failure are parsed again.

If every current record reuses metadata, the old and new counts agree, and no
unreadable entry was seen, the staging files are discarded and the live index is
left byte-for-byte unchanged. A normal rebuild action is therefore a freshness
check, not a forced metadata reread.

### Header flags

`RANKS_DEGRADED` says one or more orders fell back to walk order because a
checked sort allocation failed. Title and author each use a phase-local
`SortKey[bookCount]` allocation (14 bytes per book, 57,344 bytes at the 4,096-book
format ceiling); the first array is released before the second is requested.
Sorting is therefore best effort through the full format limit rather than
being disabled at an arbitrary library size.

`DEDUP_DEGRADED` says a directory exceeded the fixed 1024-entry duplicate-key
buffer, or that its fallible 8 KiB allocation failed. The walk still indexes
every enumerated book; it only stops remembering additional identities for
duplicate-dirent detection, so a damaged FAT may expose duplicates but cannot
make a real book disappear.

`GROUPS_DEGRADED` means the build could not allocate the arrays or buffers
needed for grouping. The configured kind and source fields are retained, with
zero group counts. See [Groups](#groups).

`selfSize` is the expected file size. Comparing it against the real one is a free
truncation guard: a build cut short by a power failure cannot pass.

## Clipping store (`/.crosspoint/clippings/epub_<path-hash>.bin`)

Version 4 retains the version 3 header and page-local range fields. After each
record's layout signature it stores `startOffset` and `endOffset` (uint32 chapter
codepoint range, end exclusive; UINT32_MAX means unavailable), `syncRevision`
(uint64), `pendingUpload` (one byte), and a 65-byte NUL-terminated sync ID. The
chapter title, text length, and text follow. Versions 1–3 remain readable.

Stable IDs are saved before upload. A sibling `.deleted` file stores fixed
65-byte IDs awaiting server acknowledgement. Deletions are queued before the
local record is removed and retried on the next enabled manual sync. A `.bak`
file is recovered if power interrupted replacement of the main store.

Clipping header strings are limited to 4 KiB on both reads and writes. A failed
load leaves no usable index and disables writes until a successful load. The
index is allocated with checked, bounded growth and released on unload.

Book moves rename the store and its `.deleted` journal (plus recovery sidecars)
together. The stored source path is informational and is refreshed on the next
save; the current file path selects the store. Local book deletion cleans up all
of these sidecars but preserves the independent `My Clippings.txt` export.

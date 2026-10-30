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

### Version 72

Version 72 makes ruby layout closer to how Chromium browser render text with
ruby characters by default (`ruby-align: space-around; ruby-overhang: auto;`)
Ruby annotation can overhang neighboring base text by at most the smaller of
half the ruby edge-character width and 1/4 of the neighboring word width. When
the ruby annotation is wider than its base text, spacing is inserted between
every character in the base text as well as at the start and the end of the
base text so that it becomes the same length as the ruby annotation.

### Version 71

Version 71 keeps the binary layout and rebuilds completed and partial sections.
Focus split offsets now locate the bold prefix in RTL words and the regular
suffix in LTR words. Book metadata and saved progress are kept.

### Version 70

Version 70 preserves the binary layout but rebuilds completed and partial
sections because SD-font layout now rounds each scaled glyph advance to pixels,
matching unkerned drawing. Cached line breaks and word positions change.
Book metadata and saved progress are kept.

### Version 69

Version 69 preserves the binary layout but rebuilds completed and partial
sections because line spacing now multiplies font size instead of the font's
newline distance (`advanceY`). Clipping layout signatures also distinguish the
new calculation so old page-local word ranges are not reused for a new layout.
Book metadata and saved progress are kept.

### Version 68

Version 68 preserves the binary layout but rebuilds completed and partial
sections to remove mid-line hyphens left by drop-cap layout. The lines beside
the drop cap use bounded greedy wrapping; the rest keep their usual wrapping.
Book metadata and saved progress are kept.

### Version 67

Version 67 preserves the binary layout but rebuilds completed and partial
sections for corrected justification of visually reordered text with attached
spaces. Book metadata and saved progress are kept.

### Version 66

Version 66 preserves the binary layout but rebuilds completed and partial
sections to remove leading non-breaking spaces carried across soft line wraps.
Explicit paragraph-leading spaces, book metadata, and saved progress are kept.

### Version 65

Version 65 preserves the binary layout but rebuilds completed and partial
sections for corrected justification of non-breaking spaces after hyphens and
ellipses. Book metadata and reading progress are unchanged.

### Version 64

Version 64 preserves the binary layout but rebuilds completed and partial
sections for corrected explicit font-weight/font-style inheritance. It retains
version 63's GIF rendering and paragraph-boundary fixes. CSS cache version 19 stores
`inherit` as value 2 in the existing font-style and font-weight bytes; the
winning declaration is resolved against the parent before applying the element's
style. Older CSS caches are rebuilt. Book metadata and reading progress are
unchanged.

### Version 63

Version 63 retains the version 60 rendering format and rebuilds completed and
partial sections to include previously omitted GIFs and their alternative text.
Earlier GIF/table development builds used versions 60 through 62; their caches
also rebuild to preserve both the current rendering format and corrected
paragraph boundaries. The partial-build sentinel remains derived from the final
section version. Book metadata and saved progress are kept, but pagination can
change as GIFs become visible.

### Version 60

The serialized layout is unchanged. Text that runs straight into a block or
heading start (`text<div>…`) now stays in its own paragraph instead of being
joined to the first word of the new block. Older completed and partial section
caches rebuild automatically; book metadata and reading progress are kept.

### Version 59

The serialized layout is unchanged. Stored CSS rules now apply even below
48 KiB free heap. Older completed and partial sections rebuild because they may
contain missing styles, different line breaks, or content that should be hidden.
Book metadata, CSS rule caches, and reading progress are kept.

### Version 58

The serialized layout is unchanged. Completed and partial sections rebuild for
corrected rendering layout and footnote placement. Book metadata and reading
progress are kept.

### Version 57

Version 57 combines the rendering layout below with line breaks after explicit
hyphens and dashes. It invalidates version 56 caches from both branches, which
used different layouts under the same version. Completed and partial sections
are rebuilt.

### Version 56

Version 56 adds the rendering layout below on top of version 55.

Each TextBlock's BlockStyle stores a signed 32-bit `fontId` after
`characterSpacing`: the renderer font the line was laid out in, or 0 for the
section font. Each TextBlock ends with a `hasWordFonts` flag. When set, it is
followed by three signed 32-bit font ids and one slot byte per word: 0 draws the
word in the block's font, `n` in font `n - 1`; bit 0x80 of a slot marks CSS
small caps (the word text keeps its original case). Mixed-size words share the baseline
of the tallest font. SD (.cpfont) and vector fonts use sized variants for
headings, CSS font-size blocks, inline runs and drop caps. A font-size span
holding a whole paragraph sizes that paragraph.

Pages gain two element types. `TAG_PageDropCap` (4) is an initial letter drawn
scaled up from a font's glyph: position (its `yPos` is the baseline of the last
spanned line), the renderer font id, the scale in 1/256ths, the word style and
up to 12 bytes of UTF-8 text. `TAG_PageBorderBox` (5) is a CSS border and/or
background shade around one page's slice of a block element: position, size,
width and style for the top, right, bottom and left edges (a side split off by a
page break has width 0), and a shade flag.

Layout changes keep headings with following content and avoid leaving a
paragraph's first line alone at a page bottom or its last line alone at a page
top. Preserved whitespace and inherited block emphasis affect word placement
and styles. Inline `<br>` breaks omit paragraph spacing, while standalone and
consecutive breaks add blank lines.

Table columns are sized from a markup pre-scan rather than equal widths.
Bordered tables frame each grid cell with `PageBorderBox` elements. Cells honor
CSS padding in grid and stacked layouts. Plain-text rows share a 160-token /
2 KB budget; rows containing blocks or images use full-width flow to preserve
paragraphs and illustrations. Older sections are rebuilt for the serialized
font data, page elements and layout changes.

CSS cache version 16 adds a boolean `preserveWhitespace` byte after `initialLetter`
and uses defined-property bit 30 for `white-space`.
CSS cache version 17 stores rules in source order and retains nonadjacent repeated
selectors so equal-specificity declarations cascade in stylesheet order.

CSS cache version 18 adds a border-component byte after the four border sides.
Bits 0-3 mark declared widths and bits 4-7 mark declared styles, in top, right,
bottom, left order. Width and style declarations cascade independently.

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

### Version 51

The serialized layout is unchanged. Paragraph continuity and top spacing are
preserved across soft flushes. Older sections are rebuilt for the updated layout.

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

#define EXPECTED_VERSION 64
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
    TAG_PageHorizontalRule = 3,
    TAG_PageDropCap = 4,
    TAG_PageBorderBox = 5
};

enum WordStyle : u8 {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,
    STRIKETHROUGH = 8,
    SUP = 16,
    SUB = 32,
    DISCRETIONARY_HYPHEN = 128  // trailing hyphen inserted by line breaking
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
    s32 fontId;
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
    u8 hasWordFonts;
    if (hasWordFonts != 0) {
        s32 wordFonts[3];
        u8 wordFontSlot[wordCount] [[comment("0 = block font, n = wordFonts[n - 1]; bit 0x80 = small caps")]];
    }
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

struct PageDropCap {
    s16 xPos;
    s16 yPos;  // baseline of the last spanned line
    s32 fontId;
    u16 scale256;
    WordStyle style;
    u8 length;
    char text[length];
};

struct BorderSide {
    u8 width;
    u8 style;  // 0 none, 1 solid, 2 double, 3 dotted, 4 dashed
};

struct PageBorderBox {
    s16 xPos;
    s16 yPos;
    u16 width;
    u16 height;
    BorderSide sides[4];  // top, right, bottom, left
    bool shaded;
};

struct PageElement {
    PageElementTag pageElementType;
    if (pageElementType == TAG_PageLine) {
        PageLine pageLine [[inline]];
    } else if (pageElementType == TAG_PageImage) {
        PageImage pageImage [[inline]];
    } else if (pageElementType == TAG_PageHorizontalRule) {
        PageHorizontalRule horizontalRule [[inline]];
    } else if (pageElementType == TAG_PageDropCap) {
        PageDropCap dropCap [[inline]];
    } else if (pageElementType == TAG_PageBorderBox) {
        PageBorderBox borderBox [[inline]];
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

Format version 2. An index written by another version fails validation on open
and is rebuilt; that is the entire migration mechanism.

### Layout

| Section | Offset | Contents |
|---|---|---|
| Header | 0 | 64 bytes, `ClixHeader` |
| Folders | `folderStart` | length-prefixed paths, one per folder |
| Records | `recordStart` | `bookCount` × 128-byte `ClixRecord` |
| Permutations | `permStart` | `bookCount` u16 author order, then `bookCount` u16 arrival order |
| Name blob | `nameStart` | per record: path hash, name, canonical author, title, source author (see below) |

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

### The name blob

Per record, at `nameStart + nameOff`:

```text
[u64 pathHash]    FNV-1a fingerprint of the complete path
[nameLen bytes]  filename, without the directory
[u8][author]     display author, one spelling chosen per authorKey across the library
[u8][title]      the book's own title, or length 0 if it never gave one
[u8][source]     cleaned author spelling before the library-wide spelling vote
```

The filename must stay the first textual field and stay the filename: `readPath`
rebuilds a book's path from it, so writing the display title there makes the book
impossible to open. That was a real defect, and it is why title has its own field.

The source author is separate from the displayed canonical author so a later
rebuild can repeat the spelling vote after books are added or removed. Existing
display reads still stop at the author or title fields and retain their offsets.

### Freshness and unchanged rebuilds

Reconciliation treats the persisted 64-bit complete-path fingerprint as the
book identity. Metadata is reused only when the fingerprint, size, nonzero FAT
timestamp, fold version, metadata mode, and expected extraction status agree.
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

# File Formats

These formats describe the SD-card cache files under `/.crosspoint/epub_<path-hash>/`.
All POD fields are written in the ESP32 little-endian representation used by
`Serialization.h`; strings are length-prefixed UTF-8.

The directory name is derived from the book path, not its content. CrossVi also
stores non-disposable reader data in this tree: `progress.bin`,
`crossvi_reader_settings.bin`, `stats_v6.bin`, `/.crosspoint/bookmarks/`,
`/.crosspoint/clippings/`, `/.crosspoint/global_stats_v4.bin`, and
`/.crosspoint/synced_stats/`. Do not treat the entire `/.crosspoint` directory
as disposable cache.

## XTC/XTCH input contract

XTC/XTCH are external fixed-layout book containers, not CrossVi cache files.
The production reader accepts the converter-verified version 1.0 subset only:
uncompressed 480×800 portrait pages, XTG 1-bit payloads inside XTC, and XTH
two-plane/four-level payloads inside XTCH. XTG uses
`ceil(width / 8) * height` bytes. Each XTH plane uses
`width * ceil(height / 8)` bytes, matching its vertical column stride.

The page table's size includes its 22-byte page header; `dataSize` in that
header is payload-only. `chapterOffset` is 64-bit and chapter page numbers are
1-based in the verified converter output. CrossVi bounds all metadata,
chapters, sizes and offsets against the physical file before allocation or
rendering. Version 0.1 byte order is not accepted because no real recommended-
converter fixture has demonstrated a need for it.

The complete contract, display policy and fixture provenance are documented in
[`lib/Xtc/README`](../lib/Xtc/README) and
[`test/xtc/resources/README.md`](../test/xtc/resources/README.md).

## Reading statistics envelope

CrossVi stores canonical reading statistics in a small integrity envelope:

- per book: `stats_v6.bin`
- local device: `/.crosspoint/global_stats_v4.bin`
- Nearby peer: `/.crosspoint/synced_stats/device_<mac>_v4.bin`
- user-created local-device backup: `/.crosspoint/stats_backups/device_stats_v1.bin`

The envelope is little-endian and has this exact layout:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `CVSE` |
| 4 | 1 | envelope version (`1`) |
| 5 | 1 | kind (`1` book, `2` global, `3` peer global) |
| 6 | 2 | payload length |
| 8 | N | versioned statistics payload |
| 8 + N | 4 | CRC32 of the complete header and payload |

The book payload is version 6 (77 bytes): it retains the readable legacy
statistics fields and adds exact start/finish minute values plus flags that
distinguish user-corrected timestamps. Versions 1–5 remain migration inputs.
Local and Nearby global payloads remain raw version 3 (159 bytes). Nearby
transmits the raw version-3 payload for interoperability and adds the envelope
only when saving the received peer snapshot.

CrossInk peer snapshots named `device_<mac>.bin{,.bak,.tmp}` are read as
legacy input and left byte-for-byte unchanged. CrossVi writes received peers to
the `_v4.bin` envelope beside them. A committed `_v4.bin` primary or backup is
authoritative; a lone damaged `_v4.bin.tmp` may fall back to the intact legacy
snapshot because that temp was never published.

Publication uses `.tmp`, sync, byte verification, rename, and decode/CRC
readback; a verified previous primary is retained as `.bak`. A corrupt primary
may recover from a valid backup or temporary file. A primary or backup v6/v4
artifact—valid or not—prevents fallback to older raw data. A lone empty,
truncated, or CRC-invalid `.tmp` is treated as an abandoned pre-publication
write and may be replaced by migration; a valid or protected temp still takes
precedence. Unknown newer envelopes, larger CRC-valid payloads, newer sibling
filenames (`stats_v7`, `global_stats_v5`, or peer `_v5` and above), wrong kinds,
and I/O errors fail closed.

On first load with no envelope artifact, CrossVi can import supported raw
`stats_v5.bin{,.bak,.tmp}`, `stats_v4.bin`, `stats.bin`, and
`global_stats.bin{,.bak,.tmp}` files. Migration never modifies or deletes those
sources. An explicit reset writes a valid zero-valued envelope (a tombstone) to
the backup first and then the primary, so a failure after the backup commit
cannot make a later primary loss resurrect retained legacy statistics.
Completion transactions likewise keep their marker until both per-book and
global primary/backup pairs contain the same committed payload. A lone invalid
transaction temp is removable pre-publication debris; a committed marker or a
newer/unreadable marker remains fail-closed.

The explicit device backup contains only the verified local global payload,
not per-book files or Nearby peer snapshots. It uses kind `2`, CRC/version
validation, `.tmp` publication and a retained `.bak`; restore republishes and
verifies both live global primary/backup copies before reporting success.

## `book.bin`

### Version 7

`book.bin` stores EPUB metadata plus lookup tables for spine and TOC entries.
The current firmware writes this version from `BookMetadataCache`.

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 7
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

### Version 31

Each file in `sections/*.bin` stores one laid-out spine section. The header is
also the cache-busting key: if any layout-affecting setting differs from the
current reader settings, the section is discarded and rebuilt.

Version 31 adds the EPUB render mode and forced-paragraph-indent values to the
cache-busting header. Version 30 had already invalidated v29 pages after Arabic
contextual shaping changed text measurement (`getTextAdvanceX` measures the
shaped visual text).

Version 28 introduced serialized word style bits for underline, strikethrough,
superscript, and subscript. The format also includes:

- cache-busting fields for paragraph alignment, hyphenation, embedded CSS,
  image rendering mode, Focus Reading, EPUB render mode, and forced paragraph
  indentation
- page offset LUT
- anchor-to-page map for fragment and footnote navigation
- paragraph and list-item LUTs used by KOReader sync page refinement
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

#define EXPECTED_VERSION 31
#define MAX_STRING_LENGTH 65535
#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 96

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
    u8 paragraphAlignment;
    u16 viewportWidth;
    u16 viewportHeight;
    bool hyphenationEnabled;
    bool embeddedStyle;
    u8 imageRendering;
    bool focusReadingEnabled;
    u8 epubRenderMode; // 0 Full, 1 Balanced, 2 Light
    bool forceParagraphIndents;

    u16 pageCount;
    u32 pageLutOffset;
    u32 anchorMapOffset;
    u32 paragraphLutOffset;
    u32 listItemLutOffset;

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
};

SectionBin section @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

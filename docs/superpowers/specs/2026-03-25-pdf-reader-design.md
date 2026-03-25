# PDF Reader Design Spec

**Date:** 2026-03-25
**Status:** Approved

---

## Goal

Add basic PDF file reading support to CrossPoint Reader for the Xteink X4 (ESP32-C3), supporting mixed documents (text + embedded images) with SD caching, outline/TOC navigation, and text reflowed through the existing rendering pipeline.

---

## Hardware Constraints

- ESP32-C3, single-core RISC-V @ 160MHz
- ~380KB usable RAM, **no PSRAM**
- 800×480 E-Ink display, 1-bit monochrome
- 16MB flash, SD card for storage
- Existing decoders: JPEGDEC, PNGdec, InflateReader/uzlib (FlateDecode)

---

## Approach

**Incremental streaming parser with SD cache** — custom minimal PDF parser in a new `lib/Pdf/` library, following the same architecture as `lib/Epub/`. Pages are parsed lazily (one at a time) and cached to `.crosspoint/pdf_<hash>/pages/<N>.bin` on the SD card. Subsequent opens read from cache.

No new external libraries. Reuses all existing infrastructure (InflateReader for decompression, JPEGDEC/PNGdec for images, GfxRenderer for text and bitmap rendering, Serialization for cache I/O).

---

## Architecture

### New library: `lib/Pdf/`

| File | Responsibility |
|------|---------------|
| `Pdf.h/.cpp` | Top-level class: open file, coordinate all sub-parsers, expose API |
| `XrefTable.h/.cpp` | Parse cross-reference table → map object IDs to byte offsets |
| `PageTree.h/.cpp` | Traverse `/Pages` tree → ordered list of page object offsets |
| `ContentStream.h/.cpp` | Parse page content stream → extract text runs + image XObject refs |
| `PdfOutline.h/.cpp` | Parse `/Outlines` tree → `OutlineEntry { title, pageNum }` list |
| `PdfPage.h/.cpp` | In-memory page representation: text blocks + image descriptors |
| `PdfCache.h/.cpp` | SD cache manager: serialize/deserialize pages and metadata |
| `StreamDecoder.h/.cpp` | FlateDecode wrapper around existing `InflateReader`/`uzlib` |

### New activities

| File | Responsibility |
|------|---------------|
| `PdfReaderActivity.h/.cpp` | Main reader UI: page display, page turn, progress save, menu |
| `PdfReaderChapterSelectionActivity.h/.cpp` | Outline/TOC list navigator (mirrors EPUB chapter selector) |

### Modified files

| File | Change |
|------|--------|
| `src/activities/reader/ReaderActivity.h/.cpp` | Add `isPdfFile()`, `loadPdf()`, route to `PdfReaderActivity` |

---

## Data Flow

### Opening a PDF

1. User selects `.pdf` in file browser
2. `ReaderActivity::loadPdf(path)` → `Pdf::open(path)` → parse xref table, page tree, outline
3. Check `.crosspoint/pdf_<hash>/meta.bin` → if miss, parse and write
4. Launch `PdfReaderActivity` with `std::unique_ptr<Pdf>`

### Rendering a page

1. `PdfReaderActivity::render()` → `Pdf::getPage(N)`
2. Check `.crosspoint/pdf_<hash>/pages/<N>.bin` → if hit, deserialize and return `PdfPage`
3. If miss:
   - Locate page content stream via xref + page tree
   - Decode FlateDecode if needed via `StreamDecoder`
   - Parse content stream: collect text runs (with Y/X position) and image XObject refs
   - Sort text runs top-to-bottom, left-to-right; group into paragraphs
   - Serialize to cache
4. `renderContents()`:
   - Text blocks → `GfxRenderer::wrappedText()` → reflow within configured margins
   - Image descriptors → extract stream → JPEGDEC/PNGdec decode → draw bitmap inline
   - Status bar: "Page 42 / 200 · Chapter Name"

---

## Cache Format

Cache root: `.crosspoint/pdf_<hash>/`
Hash: `std::hash<std::string>{}(filepath)` (same algorithm as EPUB)

### `meta.bin`

```
uint8_t  version           // format version, start at 1
uint32_t pageCount
uint32_t outlineEntryCount
[outlineEntryCount × OutlineEntry]
  uint32_t pageNum         // 0-based
  string   title           // variable length, max 256 bytes
```

### `pages/<N>.bin`

```
uint8_t  version
uint32_t textBlockCount
[textBlockCount × TextBlock]
  string   text            // UTF-8 paragraph content
  uint32_t orderHint       // Y position from PDF Tm matrix (for ordering)
uint32_t imageCount
[imageCount × ImageDescriptor]
  uint32_t pdfStreamOffset // byte offset of image stream in PDF file
  uint32_t pdfStreamLength // compressed byte length
  uint16_t width
  uint16_t height
  uint8_t  format          // 0=JPEG, 1=PNG/FlateDecode
```

### `progress.bin`

```
uint32_t currentPage
```

### Cache Invalidation

Delete cache if:
- File timestamp or size changes (checked on `Pdf::open()`)
- Screen margin or orientation changes (layout-sensitive)

---

## Text Extraction

### Content stream operators handled

| Operator | Action |
|----------|--------|
| `BT` / `ET` | Begin/end text block |
| `Tf <font> <size>` | Record for ordering only (device fonts used for rendering) |
| `Tm <a b c d e f>` | Update text matrix: X=e, Y=f for position tracking |
| `Td` / `TD` | Relative position offset |
| `T*` | New line |
| `Tj <string>` | Accumulate text |
| `TJ <array>` | Accumulate text (skip numeric kerning values) |
| `'` / `"` | New line + show text |
| `Do <name>` | Record image XObject reference |
| Everything else | Skip |

### Text ordering

- Each text run stored with Y position from Tm matrix
- After full page scan: sort descending Y (top to bottom), then ascending X
- Group runs within ±(lineHeight/2) of each other → same paragraph
- Concatenate grouped runs into a single `TextBlock`

### Encoding

- Try `/ToUnicode` CMap first (covers ~95% of modern PDFs)
- Fall back to WinAnsi-1252 for standard 14 fonts
- Unknown bytes → U+FFFD replacement character

---

## Image Handling

### Detection

- `Do <name>` operator → look up in page `/Resources /XObject` dict
- Check `/Subtype /Image`

### Supported formats

| PDF Filter | Handled by |
|-----------|-----------|
| `/DCTDecode` | JPEGDEC (already in project) |
| `/FlateDecode` + DeviceGray/DeviceRGB | PNGdec or raw bitmap |
| CCITT, LZW, JBIG2 | Skip — log warning, draw `[image]` placeholder |

### Rendering

- Scale to fit column width (maintain aspect ratio)
- Draw inline between surrounding text blocks
- On decode failure: draw `[image]` placeholder text, continue

### RAM

- Images decoded one at a time, never held in RAM simultaneously
- Working buffers owned by JPEGDEC/PNGdec (already tuned for 380KB constraint)

---

## Outline / TOC Navigation

### Parsing

- PDF `/Outlines` → linked list of outline items
- Each item: `/Title` (string) + `/Dest` or `/A` → page reference
- Parse recursively, flatten to a `std::vector<OutlineEntry>` capped at 256 entries
- Cache in `meta.bin`

### UI

- `PdfReaderChapterSelectionActivity` mirrors `EpubReaderChapterSelectionActivity`
- `ButtonNavigator` for up/down/confirm
- Shows (title, page number) per entry
- On confirm: jump to that page in `PdfReaderActivity`

### Menu

- Reader menu (same button as EPUB): [Outline], [Jump to Page], [Settings], [Home]

---

## Progress & Settings Integration

- Font, font size, margins, line spacing, orientation: all inherited from `SETTINGS` (same as EPUB/TXT)
- No PDF-specific settings
- Progress: debounced write to `progress.bin` (same guard as EPUB: only write when page changes)
- Progress loaded in `onEnter()`, saved in `onExit()` + on page change

---

## Error Handling

| Scenario | Behaviour |
|----------|-----------|
| File can't be opened | Show error dialog: "Failed to open PDF" |
| Missing/invalid xref | Log `LOG_ERR`, return nullptr from `Pdf::open()` |
| Unsupported PDF version | Log warning, attempt parse anyway |
| Truncated content stream | Render partial page, log warning |
| Image decode failure | Draw `[image]` placeholder, continue |
| Cache corruption | Delete cache file, re-parse on next access |
| malloc failure | `LOG_ERR` + return false (consistent with project pattern) |

All errors use `LOG_ERR("PDF", ...)` / `LOG_DBG("PDF", ...)`. No exceptions.

---

## Out of Scope

- Encrypted PDFs (password protection)
- PDF forms / interactive fields
- Right-to-left text (Arabic, Hebrew)
- CJK fonts / CIDFont with complex encoding
- Multi-column layout preservation
- Vector graphics / path rendering
- PDF annotations

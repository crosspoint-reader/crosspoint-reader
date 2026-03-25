# PDF Reader Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add basic PDF reading support (text + images, outline/TOC navigation, SD caching) to CrossPoint Reader for the Xteink X4 (ESP32-C3).

**Architecture:** Custom minimal PDF parser in `lib/Pdf/` following the existing `lib/Epub/` pattern. Pages parsed lazily, cached to `.crosspoint/pdf_<hash>/` on SD. Text reflowed through existing GfxRenderer pipeline. Images decoded via existing JPEGDEC/PNGdec.

**Tech Stack:** C++20, PlatformIO, ESP32-C3 Arduino, SdFat (via HalStorage), existing InflateReader/uzlib, JPEGDEC, PNGdec, GfxRenderer, Serialization lib.

**Spec:** `docs/superpowers/specs/2026-03-25-pdf-reader-design.md`

**Build command:** `pio run -e default` (must exit 0 after every task)

**Hardware notes:**
- Stack allocations > 256 bytes → use `malloc` + null check + `free`
- No `std::string` in hot render paths — use `string_view` or `char[]`
- No `std::shared_ptr` — use `std::unique_ptr`
- No exceptions (`-fno-exceptions` build flag)
- All user-facing strings via `tr(STR_*)` macro

---

## File Map

### New files

```
lib/Pdf/
  Pdf.h                                        Top-level PDF class (public API)
  Pdf.cpp
  Pdf/XrefTable.h                              Cross-reference table parser
  Pdf/XrefTable.cpp
  Pdf/PageTree.h                               Page tree traversal
  Pdf/PageTree.cpp
  Pdf/ContentStream.h                          Content stream text+image extractor
  Pdf/ContentStream.cpp
  Pdf/PdfOutline.h                             Outline/bookmark parser
  Pdf/PdfOutline.cpp
  Pdf/PdfPage.h                                In-memory page data (text blocks + image descriptors)
  Pdf/PdfPage.cpp
  Pdf/PdfCache.h                               SD cache read/write
  Pdf/PdfCache.cpp
  Pdf/StreamDecoder.h                          FlateDecode wrapper (wraps InflateReader)
  Pdf/StreamDecoder.cpp

src/activities/reader/
  PdfReaderActivity.h                          Main reader UI
  PdfReaderActivity.cpp
  PdfReaderChapterSelectionActivity.h          TOC navigator
  PdfReaderChapterSelectionActivity.cpp
```

### Modified files

```
src/activities/reader/ReaderActivity.h         Add isPdfFile(), loadPdf() declarations
src/activities/reader/ReaderActivity.cpp       Add PDF routing logic
lib/I18n/translations/english.yaml            Add STR_PDF_LOAD_ERROR, STR_PDF_IMAGE_PLACEHOLDER
```

---

## Task 1: Project skeleton — lib/Pdf headers and empty stubs

**Goal:** Get the file structure in place and verify it compiles as empty stubs.

**Files:**
- Create: all `lib/Pdf/` headers and `.cpp` stubs listed in File Map above

- [ ] **1.1 Create `lib/Pdf/Pdf/PdfPage.h`**

```cpp
#pragma once
#include <string>
#include <vector>

struct PdfTextBlock {
  std::string text;
  uint32_t orderHint = 0;  // Y position from PDF (descending = top)
};

struct PdfImageDescriptor {
  uint32_t pdfStreamOffset = 0;
  uint32_t pdfStreamLength = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t format = 0;  // 0=JPEG, 1=PNG/FlateDecode
};

struct PdfPage {
  std::vector<PdfTextBlock> textBlocks;
  std::vector<PdfImageDescriptor> images;
};
```

- [ ] **1.2 Note on `PdfOutlineEntry`**

`PdfOutlineEntry` is defined directly inside `lib/Pdf/Pdf/PdfOutline.h` (see Task 1.11) alongside `PdfOutlineParser`. There is **no** separate `PdfOutlineEntry.h` file. Both `PdfCache.h` and any other file that needs the struct should `#include "PdfOutline.h"` — not `"PdfOutlineEntry.h"`.

- [ ] **1.3 Create `lib/Pdf/Pdf/XrefTable.h`**

```cpp
#pragma once
#include <HalStorage.h>
#include <vector>

class XrefTable {
  std::vector<uint32_t> offsets;  // offsets[objId] = byte offset in file
 public:
  bool parse(FsFile& file);
  uint32_t getOffset(uint32_t objId) const;
  uint32_t objectCount() const;
};
```

- [ ] **1.4 Create `lib/Pdf/Pdf/XrefTable.cpp`** (empty stub returning false/0)

```cpp
#include "XrefTable.h"
#include <Logging.h>

bool XrefTable::parse(FsFile& file) { return false; }
uint32_t XrefTable::getOffset(uint32_t objId) const { return 0; }
uint32_t XrefTable::objectCount() const { return 0; }
```

- [ ] **1.5 Create `lib/Pdf/Pdf/PageTree.h`**

```cpp
#pragma once
#include <HalStorage.h>
#include <vector>
#include "XrefTable.h"

class PageTree {
  std::vector<uint32_t> pageOffsets;  // byte offset of each page object
 public:
  bool parse(FsFile& file, const XrefTable& xref, uint32_t rootObjId);
  uint32_t pageCount() const;
  uint32_t getPageOffset(uint32_t pageIndex) const;
};
```

- [ ] **1.6 Create `lib/Pdf/Pdf/PageTree.cpp`** (empty stub)

```cpp
#include "PageTree.h"

bool PageTree::parse(FsFile&, const XrefTable&, uint32_t) { return false; }
uint32_t PageTree::pageCount() const { return 0; }
uint32_t PageTree::getPageOffset(uint32_t) const { return 0; }
```

- [ ] **1.7 Create `lib/Pdf/Pdf/StreamDecoder.h`**

```cpp
#pragma once
#include <HalStorage.h>

// Wraps InflateReader/uzlib to FlateDecode a PDF stream.
// Reads up to `maxOutBytes` of decompressed data into `outBuf`.
// Returns number of bytes written, or 0 on error.
class StreamDecoder {
 public:
  static size_t flateDecode(FsFile& file, uint32_t streamOffset,
                            uint32_t compressedLen, uint8_t* outBuf,
                            size_t maxOutBytes);
};
```

- [ ] **1.8 Create `lib/Pdf/Pdf/StreamDecoder.cpp`** (empty stub)

```cpp
#include "StreamDecoder.h"

size_t StreamDecoder::flateDecode(FsFile&, uint32_t, uint32_t, uint8_t*, size_t) {
  return 0;
}
```

- [ ] **1.9 Create `lib/Pdf/Pdf/ContentStream.h`**

```cpp
#pragma once
#include <HalStorage.h>
#include "PdfPage.h"
#include "XrefTable.h"

class ContentStream {
 public:
  // Parse a page content stream starting at `streamOffset` with `streamLen` bytes.
  // Fills `outPage` with extracted text blocks and image descriptors.
  static bool parse(FsFile& file, uint32_t streamOffset, uint32_t streamLen,
                    bool isCompressed, const XrefTable& xref,
                    PdfPage& outPage);
};
```

- [ ] **1.10 Create `lib/Pdf/Pdf/ContentStream.cpp`** (empty stub)

```cpp
#include "ContentStream.h"

bool ContentStream::parse(FsFile&, uint32_t, uint32_t, bool, const XrefTable&, PdfPage&) {
  return false;
}
```

- [ ] **1.11 Create `lib/Pdf/Pdf/PdfOutline.h`**

```cpp
#pragma once
#include <HalStorage.h>
#include <vector>
#include "XrefTable.h"
// Note: PdfOutlineEntry is defined directly in PdfOutline.h (no separate header needed)

struct PdfOutlineEntry {
  std::string title;
  uint32_t pageNum = 0;
};

class PdfOutlineParser {
 public:
  static bool parse(FsFile& file, const XrefTable& xref, uint32_t outlinesObjId,
                    std::vector<PdfOutlineEntry>& outEntries);
};
```

- [ ] **1.12 Create `lib/Pdf/Pdf/PdfOutline.cpp`** (empty stub)

```cpp
#include "PdfOutline.h"

bool PdfOutlineParser::parse(FsFile&, const XrefTable&, uint32_t,
                             std::vector<PdfOutlineEntry>&) {
  return false;
}
```

- [ ] **1.13 Create `lib/Pdf/Pdf/PdfCache.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include "PdfPage.h"
#include "PdfOutline.h"  // PdfOutlineEntry is defined in PdfOutline.h (no separate PdfOutlineEntry.h)

class PdfCache {
  std::string cacheDir;
 public:
  explicit PdfCache(const std::string& pdfFilePath);
  bool loadMeta(uint32_t& pageCount, std::vector<PdfOutlineEntry>& outline);
  bool saveMeta(uint32_t pageCount, const std::vector<PdfOutlineEntry>& outline);
  bool loadPage(uint32_t pageNum, PdfPage& outPage);
  bool savePage(uint32_t pageNum, const PdfPage& page);
  bool loadProgress(uint32_t& currentPage);
  bool saveProgress(uint32_t currentPage);
  void invalidate();  // delete all cache files
  const std::string& getCacheDir() const;
};
```

- [ ] **1.14 Create `lib/Pdf/Pdf/PdfCache.cpp`** (empty stub returning false)

```cpp
#include "PdfCache.h"
#include <Logging.h>

PdfCache::PdfCache(const std::string& pdfFilePath) {
  size_t hash = std::hash<std::string>{}(pdfFilePath);
  char buf[64];
  snprintf(buf, sizeof(buf), "/.crosspoint/pdf_%zu", hash);
  cacheDir = buf;
}

bool PdfCache::loadMeta(uint32_t&, std::vector<PdfOutlineEntry>&) { return false; }
bool PdfCache::saveMeta(uint32_t, const std::vector<PdfOutlineEntry>&) { return false; }
bool PdfCache::loadPage(uint32_t, PdfPage&) { return false; }
bool PdfCache::savePage(uint32_t, const PdfPage&) { return false; }
bool PdfCache::loadProgress(uint32_t& p) { p = 0; return false; }
bool PdfCache::saveProgress(uint32_t) { return false; }
void PdfCache::invalidate() {}
const std::string& PdfCache::getCacheDir() const { return cacheDir; }
```

- [ ] **1.15 Create `lib/Pdf/Pdf.h`**

```cpp
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "Pdf/PdfPage.h"
#include "Pdf/PdfOutline.h"

class Pdf {
  struct Impl;
  std::unique_ptr<Impl> impl;

  explicit Pdf(std::unique_ptr<Impl> impl);
 public:
  ~Pdf();

  // Opens a PDF file. Returns nullptr on failure.
  static std::unique_ptr<Pdf> open(const std::string& path);

  uint32_t pageCount() const;
  const std::vector<PdfOutlineEntry>& outline() const;
  const std::string& filePath() const;

  // Returns a parsed page (from cache or freshly parsed).
  // Returns nullptr on failure.
  std::unique_ptr<PdfPage> getPage(uint32_t pageNum);

  // Progress persistence — delegates to impl->cache internally.
  // External callers (PdfReaderActivity) must NOT access impl->cache directly.
  bool saveProgress(uint32_t page);
  bool loadProgress(uint32_t& page);
};
```

- [ ] **1.16 Create `lib/Pdf/Pdf.cpp`** (empty stub)

```cpp
#include "Pdf.h"
#include <Logging.h>

struct Pdf::Impl {
  std::string path;
  uint32_t pages = 0;
  std::vector<PdfOutlineEntry> outlineEntries;
};

Pdf::Pdf(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
Pdf::~Pdf() = default;

std::unique_ptr<Pdf> Pdf::open(const std::string& path) {
  LOG_INF("PDF", "open: %s", path.c_str());
  return nullptr;  // stub
}
uint32_t Pdf::pageCount() const { return impl ? impl->pages : 0; }
const std::vector<PdfOutlineEntry>& Pdf::outline() const {
  static std::vector<PdfOutlineEntry> empty;
  return impl ? impl->outlineEntries : empty;
}
const std::string& Pdf::filePath() const {
  static std::string empty;
  return impl ? impl->path : empty;
}
bool Pdf::saveProgress(uint32_t page) {
  return impl && impl->cache ? impl->cache->saveProgress(page) : false;
}
bool Pdf::loadProgress(uint32_t& page) {
  return impl && impl->cache ? impl->cache->loadProgress(page) : false;
}
}
std::unique_ptr<PdfPage> Pdf::getPage(uint32_t) { return nullptr; }
```

- [ ] **1.17 Verify build succeeds**

```bash
pio run -e default 2>&1 | tail -5
```

Expected: `[SUCCESS]` with no errors. Fix any compile errors before proceeding.

- [ ] **1.18 Commit**

```bash
git add lib/Pdf/
git commit -m "feat(pdf): add lib/Pdf skeleton with empty stubs"
```

---

## Task 2: XRef table parser

**Goal:** Parse a real PDF cross-reference table so we can locate any PDF object by ID.

**Files:**
- Modify: `lib/Pdf/Pdf/XrefTable.h`, `lib/Pdf/Pdf/XrefTable.cpp`

**Background:** A PDF xref table starts at the byte offset given by `startxref` near the end of the file. Format:
```
xref
0 6
0000000000 65535 f
0000000009 00000 n
...
```
Each entry is 20 bytes: 10-digit offset, space, 5-digit generation, space, `f` (free) or `n` (in-use), `\r\n`.
After the xref table comes a `trailer` dict containing `/Root`, `/Size`, `/Info`, `/Encrypt`.

Cross-reference streams (PDF 1.5+) use compressed streams — skip for v1, just parse traditional xref tables.

- [ ] **2.1 Implement `XrefTable::parse()`**

```cpp
bool XrefTable::parse(FsFile& file) {
  // 1. Seek to end - 1024 bytes, scan for "startxref"
  // 2. Read the offset value after "startxref"
  // 3. Seek to that offset
  // 4. Read "xref" keyword
  // 5. Loop: read subsection header "firstObj count", then count*20-byte entries
  //    - entry format: "nnnnnnnnnn ggggg n\r\n" (20 bytes)
  //    - if 'n' (in-use): offsets[firstObj + i] = parsed 10-digit offset
  //    - if 'f' (free): offsets[firstObj + i] = 0
  // 6. After xref table, scan for "trailer" dict
  //    - extract /Root <N G R> -> store rootObjId
  //    - extract /Size -> reserve offsets vector
  // 7. Return true if at least one object found
}
```

Key implementation notes:
- Read 1024-byte tail buffer once, **manual byte scan only** for `startxref` — `memmem()` is not available on ESP32-C3. Write a simple loop scanning for the byte sequence `{'s','t','a','r','t','x','r','e','f'}`.
- Use `file.seek()` + `file.read()` (via `HalStorage`/SdFat `FsFile`)
- Parse numbers with `strtoul()` on a stack `char[32]` buffer, never `std::string`
- `offsets` vector: call `.reserve(size)` from `/Size` before filling
- Store root object ID as a member for `Pdf::open()` to retrieve

- [ ] **2.2 Add `rootObjId()` accessor to `XrefTable.h`**

```cpp
uint32_t rootObjId() const;
```

- [ ] **2.3 Build and verify**

```bash
pio run -e default 2>&1 | tail -5
```

Expected: `[SUCCESS]`

- [ ] **2.4 Commit**

```bash
git add lib/Pdf/Pdf/XrefTable.h lib/Pdf/Pdf/XrefTable.cpp
git commit -m "feat(pdf): implement XrefTable parser"
```

---

## Task 3: PDF object reader helper

**Goal:** A helper that reads a PDF object at a given byte offset (returns its type and raw value string).

**Files:**
- Create: `lib/Pdf/Pdf/PdfObject.h`, `lib/Pdf/Pdf/PdfObject.cpp`

**Background:** PDF object format: `<id> <gen> obj ... endobj`. Object body can be: dictionary `<< /Key Value >>`, stream (dictionary + `stream\r\n...endstream`), number, string `(...)`, name `/Name`, array `[...]`, boolean, null, indirect reference `N G R`.

This helper is used by PageTree, ContentStream, and PdfOutline to look up objects.

- [ ] **3.1 Create `lib/Pdf/Pdf/PdfObject.h`**

```cpp
#pragma once
#include <HalStorage.h>
#include <string>

// Minimal PDF object reader.
// Seeks to offset, reads past "id gen obj", returns the raw body as a string.
// For stream objects: bodyStr contains the dictionary, streamOffset is set.
class PdfObject {
 public:
  // Read object at `offset`. Returns false if not a valid object.
  static bool readAt(FsFile& file, uint32_t offset, std::string& bodyStr,
                     uint32_t* streamOffset = nullptr,
                     uint32_t* streamLength = nullptr);

  // Extract a scalar value from a dict string: e.g. getDictValue("/Root", dict)
  static std::string getDictValue(const char* key, const std::string& dict);

  // Extract integer from dict value string.
  static int getDictInt(const char* key, const std::string& dict, int defaultVal = 0);

  // Extract indirect reference object ID: "/Key N G R" -> N
  static uint32_t getDictRef(const char* key, const std::string& dict);
};
```

- [ ] **3.2 Implement `PdfObject.cpp`**

Key notes:
- `readAt()`: seek to offset, skip `"<id> <gen> obj"`, then read up to 4096 bytes using a **heap buffer** (a 4096-byte stack buffer is a stack overflow risk on ESP32-C3 where the default task stack is small):
  ```cpp
  auto* buf = static_cast<uint8_t*>(malloc(4096));
  if (!buf) { LOG_ERR("PDF", "malloc failed"); return false; }
  // ... read into buf, copy to bodyStr ...
  free(buf);
  buf = nullptr;
  ```
- `getDictValue()`: simple linear scan for key, return bytes until next whitespace or `>>`
- This is the most error-prone parser — be defensive: bounds-check every index, never read past buffer end
- `streamLength`: look for `/Length` in the dict, use `getDictInt("/Length", dict, 0)`
- `streamOffset`: offset of first byte AFTER `stream\r\n` keyword

- [ ] **3.3 Build and verify**

```bash
pio run -e default 2>&1 | tail -5
```

- [ ] **3.4 Commit**

```bash
git add lib/Pdf/Pdf/PdfObject.h lib/Pdf/Pdf/PdfObject.cpp
git commit -m "feat(pdf): add PdfObject helper for reading raw objects"
```

---

## Task 4: Page tree and StreamDecoder

**Goal:** Implement page tree traversal so we can find the byte offset of page N, and implement FlateDecode decompression.

**Files:**
- Modify: `lib/Pdf/Pdf/PageTree.cpp`
- Modify: `lib/Pdf/Pdf/StreamDecoder.cpp`

### 4A: PageTree

**Background:** PDF page tree is a tree of `/Pages` (intermediate) and `/Page` (leaf) nodes. Root is the `/Pages` object referenced by `/Root`'s `/Pages` key. Each node has `/Type /Pages` or `/Type /Page`, `/Kids [refs]`, `/Count`.

- [ ] **4A.1 Implement `PageTree::parse()`**

```cpp
bool PageTree::parse(FsFile& file, const XrefTable& xref, uint32_t rootObjId) {
  // rootObjId is the /Pages object (from PdfObject::getDictRef("/Pages", catalogDict))
  // Recursively (or iteratively) traverse the tree:
  //   - read object at xref.getOffset(id)
  //   - if /Type /Pages: iterate /Kids array, recurse
  //   - if /Type /Page: push xref offset of this page to pageOffsets
  // Use iterative DFS with a stack<uint32_t> to avoid call-stack depth
  // Cap at 9999 pages for safety
}
```

- [ ] **4A.2 Build and verify**

```bash
pio run -e default 2>&1 | tail -5
```

### 4B: StreamDecoder

**Background:** Most PDF content streams use `/Filter /FlateDecode` (zlib deflate). The project already has `InflateReader` in `lib/InflateReader/`. Check its API before implementing.

- [ ] **4B.1 Read the InflateReader API**

Read `lib/InflateReader/InflateReader.h` fully before writing StreamDecoder.

- [ ] **4B.2 Implement `StreamDecoder::flateDecode()`**

```cpp
size_t StreamDecoder::flateDecode(FsFile& file, uint32_t streamOffset,
                                  uint32_t compressedLen,
                                  uint8_t* outBuf, size_t maxOutBytes) {
  // 1. file.seek(streamOffset)
  // 2. Allocate a read buffer on stack or heap (e.g. 1024 bytes at a time)
  // 3. Use InflateReader to decompress into outBuf
  // 4. Return bytes written
  // 5. ALWAYS check malloc return values
}
```

- [ ] **4B.3 Build and verify**

```bash
pio run -e default 2>&1 | tail -5
```

- [ ] **4B.4 Commit**

```bash
git add lib/Pdf/Pdf/PageTree.cpp lib/Pdf/Pdf/StreamDecoder.cpp
git commit -m "feat(pdf): implement PageTree traversal and StreamDecoder FlateDecode"
```

---

## Task 5: Content stream parser (text + images)

**Goal:** Parse a PDF page content stream and extract text blocks with position hints and image XObject references.

**Files:**
- Modify: `lib/Pdf/Pdf/ContentStream.cpp`

**Background:** PDF content streams are a series of operands followed by an operator. Text operators are enclosed in `BT`...`ET` blocks. The text matrix set by `Tm a b c d e f` gives X=e, Y=f position.

- [ ] **5.1 Implement tokenizer**

Write a simple tokenizer that reads bytes from a buffer and returns:
- Numbers (integer or float as string)
- Names (`/Name`)
- Strings (`(text)` or `<hex>`)
- Arrays (`[...]` contents)
- Operators (any other token)

Keep it as a simple state machine. No heap allocation — work on a 4096-byte sliding window buffer.

- [ ] **5.2 Implement text operator handling**

```cpp
// State for the text extraction loop:
float textX = 0, textY = 0;         // current text position
float lineSpacing = 0;
std::string currentTextRun;
std::vector<PdfTextBlock> runs;     // collected before sorting

// On BT: reset textX/textY
// On Tm a b c d e f: textX=e, textY=f
// On Td dx dy: textX+=dx, textY+=dy
// On TD dx dy: lineSpacing=-dy; textX+=dx; textY+=dy
// On T*: textY -= lineSpacing
// On Tj (str): append decoded str to currentTextRun, push run with Y=textY
// On TJ [array]: for each string in array, append; for numbers, skip
// On ET: push any remaining currentTextRun
```

- [ ] **5.3 Implement text encoding decode**

```cpp
// Try to decode PDF string bytes to UTF-8:
// 1. If string starts with BOM (0xFE 0xFF): UTF-16BE encoded → convert
// 2. Otherwise: treat as WinAnsi-1252 → map to Unicode
// Store a simple static const uint16_t win1252ToUnicode[256] table
// Then encode each codepoint as UTF-8 into a char[] buffer
```

WinAnsi-1252 to Unicode mapping for non-ASCII bytes (0x80–0xFF) is a standard table — include it as a `static constexpr uint16_t` array in the .cpp file.

- [ ] **5.4 Implement text sorting and paragraph grouping**

```cpp
// After all BT/ET blocks processed:
// 1. Sort runs by orderHint descending (high Y = top of page)
// 2. Group: if |run[i].orderHint - run[i-1].orderHint| < lineThreshold → same block
//    lineThreshold = 20 (roughly 1 line height in PDF units)
// 3. Concatenate grouped runs with space separator → one PdfTextBlock per group
// 4. outPage.textBlocks = result
```

- [ ] **5.5 Implement Do operator (image reference)**

```cpp
// On Do /Name:
// 1. Look up /Name in XObject dict of current page resources
// 2. Read the XObject object at xref.getOffset(xobjectObjId)
// 3. Check /Subtype /Image
// 4. Get /Width, /Height, /Filter (/DCTDecode=JPEG, /FlateDecode=PNG-like)
// 5. Push PdfImageDescriptor { streamOffset, streamLength, width, height, format }
```

- [ ] **5.6 Build and verify**

```bash
pio run -e default 2>&1 | tail -5
```

- [ ] **5.7 Commit**

```bash
git add lib/Pdf/Pdf/ContentStream.cpp
git commit -m "feat(pdf): implement content stream parser for text and image extraction"
```

---

## Task 6: Outline parser and PdfCache

**Goal:** Parse PDF bookmarks and implement SD read/write cache.

**Files:**
- Modify: `lib/Pdf/Pdf/PdfOutline.cpp`, `lib/Pdf/Pdf/PdfCache.cpp`

### 6A: Outline parser

- [ ] **6A.1 Implement `PdfOutlineParser::parse()`**

```cpp
// 1. Read /Outlines object at xref.getOffset(outlinesObjId)
// 2. Follow /First pointer to first child
// 3. For each child: read /Title (PDF string → UTF-8), resolve /Dest or /A to page num
//    - /Dest [pageRef ...] → resolve pageRef to page index via PageTree
//    - /A << /Type /Action /S /GoTo /D [...] >> → same
// 4. Follow /Next pointer for siblings
// 5. Recurse into /First of each item for children (flatten to a single list)
// 6. Cap at 256 entries total
// 7. If /Outlines missing → return true with empty list (not an error)
```

### 6B: PdfCache — implement SD read/write

- [ ] **6B.1 Implement `PdfCache::saveMeta()` and `loadMeta()`**

Use `Serialization` library (same as `BookMetadataCache` in `lib/Epub/`). Read `lib/Serialization/` to understand the API before writing.

Format:
```
uint8_t  version = 1
uint32_t pageCount
uint32_t outlineCount
[outlineCount × { uint32_t pageNum, string title }]
```

- [ ] **6B.2 Implement `PdfCache::savePage()` and `loadPage()`**

Format:
```
uint8_t  version = 1
uint32_t textBlockCount
[textBlockCount × { string text, uint32_t orderHint }]
uint32_t imageCount
[imageCount × { uint32_t offset, uint32_t len, uint16_t w, uint16_t h, uint8_t fmt }]
```

Page file path: `<cacheDir>/pages/<N>.bin` — ensure directory is created.

- [ ] **6B.3 Implement `PdfCache::saveProgress()` and `loadProgress()`**

File: `<cacheDir>/progress.bin`, content: single `uint32_t currentPage`.

- [ ] **6B.4 Implement `PdfCache::invalidate()`**

Delete all files in `<cacheDir>/` recursively using `HalStorage`/SdFat APIs.

- [ ] **6B.5 Build and verify**

```bash
pio run -e default 2>&1 | tail -5
```

- [ ] **6B.6 Commit**

```bash
git add lib/Pdf/Pdf/PdfOutline.cpp lib/Pdf/Pdf/PdfCache.cpp
git commit -m "feat(pdf): implement outline parser and SD cache read/write"
```

---

## Task 7: Pdf.cpp — wire everything together

**Goal:** Implement `Pdf::open()` and `Pdf::getPage()` using all the components built above.

**Files:**
- Modify: `lib/Pdf/Pdf.cpp`, `lib/Pdf/Pdf.h`

- [ ] **7.1 Expand `Pdf::Impl`**

```cpp
struct Pdf::Impl {
  std::string path;
  FsFile file;                         // kept open for lazy page reads
  XrefTable xref;
  PageTree pageTree;
  std::vector<PdfOutlineEntry> outlineEntries;
  std::unique_ptr<PdfCache> cache;
};
```

- [ ] **7.2 Implement `Pdf::open()`**

```cpp
std::unique_ptr<Pdf> Pdf::open(const std::string& path) {
  auto impl = std::make_unique<Impl>();
  impl->path = path;

  if (!Storage.openFileForRead("PDF", path, impl->file)) {
    LOG_ERR("PDF", "Cannot open: %s", path.c_str());
    return nullptr;
  }

  if (!impl->xref.parse(impl->file)) {
    LOG_ERR("PDF", "Failed to parse xref: %s", path.c_str());
    return nullptr;
  }

  // Read catalog object (/Root) → get /Pages ref
  std::string catalogBody;
  if (!PdfObject::readAt(impl->file, impl->xref.getOffset(impl->xref.rootObjId()), catalogBody)) {
    return nullptr;
  }
  uint32_t pagesObjId = PdfObject::getDictRef("/Pages", catalogBody);

  if (!impl->pageTree.parse(impl->file, impl->xref, pagesObjId)) {
    LOG_ERR("PDF", "Failed to parse page tree");
    return nullptr;
  }

  impl->cache = std::make_unique<PdfCache>(path);

  // Load outline from cache or parse
  uint32_t cachedPageCount = 0;
  if (!impl->cache->loadMeta(cachedPageCount, impl->outlineEntries)) {
    uint32_t outlinesId = PdfObject::getDictRef("/Outlines", catalogBody);
    if (outlinesId != 0) {
      PdfOutlineParser::parse(impl->file, impl->xref, outlinesId, impl->outlineEntries);
    }
    impl->cache->saveMeta(impl->pageTree.pageCount(), impl->outlineEntries);
  }

  return std::unique_ptr<Pdf>(new Pdf(std::move(impl)));
}
```

- [ ] **7.3 Implement `Pdf::getPage()`**

```cpp
std::unique_ptr<PdfPage> Pdf::getPage(uint32_t pageNum) {
  auto page = std::make_unique<PdfPage>();

  if (impl->cache->loadPage(pageNum, *page)) {
    return page;  // cache hit
  }

  // Cache miss: parse from file
  uint32_t pageObjOffset = impl->pageTree.getPageOffset(pageNum);
  std::string pageBody;
  uint32_t streamOffset = 0, streamLen = 0;
  if (!PdfObject::readAt(impl->file, pageObjOffset, pageBody, &streamOffset, &streamLen)) {
    return nullptr;
  }

  bool compressed = pageBody.find("/FlateDecode") != std::string::npos ||
                    pageBody.find("/Fl ") != std::string::npos;

  if (!ContentStream::parse(impl->file, streamOffset, streamLen,
                             compressed, impl->xref, *page)) {
    LOG_ERR("PDF", "Failed to parse page %u", pageNum);
    return nullptr;
  }

  impl->cache->savePage(pageNum, *page);
  return page;
}
```

- [ ] **7.4 Build and verify**

```bash
pio run -e default 2>&1 | tail -5
```

- [ ] **7.5 Commit**

```bash
git add lib/Pdf/Pdf.h lib/Pdf/Pdf.cpp
git commit -m "feat(pdf): wire Pdf::open() and Pdf::getPage() with all sub-parsers"
```

---

## Task 8: PdfReaderActivity — basic text rendering

**Goal:** A working activity that displays PDF pages as reflowed text (no images yet).

**Files:**
- Create: `src/activities/reader/PdfReaderActivity.h`
- Create: `src/activities/reader/PdfReaderActivity.cpp`

- [ ] **8.1 Create `PdfReaderActivity.h`**

```cpp
#pragma once
#include <Pdf.h>
#include "activities/Activity.h"

class PdfReaderActivity final : public Activity {
  std::unique_ptr<Pdf> pdf;
  uint32_t currentPage = 0;
  uint32_t totalPages = 0;
  int pagesUntilFullRefresh = 0;

  // Progress debounce (same pattern as EPUB reader)
  uint32_t lastSavedPage = UINT32_MAX;

  void renderContents(const PdfPage& page);
  void renderStatusBar() const;
  void saveProgress();

 public:
  explicit PdfReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             std::unique_ptr<Pdf> pdf);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
};
```

- [ ] **8.2 Implement `PdfReaderActivity.cpp`**

Follow the `TxtReaderActivity` pattern closely. Key points:
- `onEnter()`: load progress via `pdf->loadProgress(currentPage)` (public API — **do not** access `pdf->cache` directly since `cache` is private inside `Pdf::Impl`), compute `totalPages = pdf->pageCount()`
- `loop()`: handle button input via `MappedInputManager` — `Button::PageForward` / `Button::PageBack` → increment/decrement `currentPage`, call `requestRender()`
- `render()`: call `pdf->getPage(currentPage)`, if null show error, else call `renderContents()`
- `renderContents()`: iterate `page.textBlocks`, call `GUI.drawWrappedText(...)` or `renderer.wrappedText()` + `renderer.drawText()` for each block
- `renderStatusBar()`: draw "Page N / M" at bottom using `GUI` macros (see Task 12 for correct snprintf pattern)
- `saveProgress()`: call `pdf->saveProgress(currentPage)` (public method on `Pdf` — **do not** access `pdf->cache` directly) only if `currentPage != lastSavedPage`
- Full refresh every 10 page turns (`pagesUntilFullRefresh`)
- `onExit()`: `saveProgress()`, reset state

- [ ] **8.3 Build and verify**

```bash
pio run -e default 2>&1 | tail -5
```

- [ ] **8.4 Commit**

```bash
git add src/activities/reader/PdfReaderActivity.h src/activities/reader/PdfReaderActivity.cpp
git commit -m "feat(pdf): add PdfReaderActivity with text-only page rendering"
```

---

## Task 9: ReaderActivity integration

**Goal:** Wire up PDF detection and routing in the existing `ReaderActivity`.

**Files:**
- Modify: `src/activities/reader/ReaderActivity.h`
- Modify: `src/activities/reader/ReaderActivity.cpp`

- [ ] **9.1 Add to `ReaderActivity.h`**

```cpp
#include <Pdf.h>
class PdfReaderActivity;

// Inside ReaderActivity class:
static std::unique_ptr<Pdf> loadPdf(const std::string& path);
static bool isPdfFile(const std::string& path);
void onGoToPdfReader(std::unique_ptr<Pdf> pdf);
```

- [ ] **9.2 Add to `ReaderActivity.cpp`**

```cpp
bool ReaderActivity::isPdfFile(const std::string& path) {
  if (path.size() < 4) return false;
  std::string ext = path.substr(path.size() - 4);
  // lowercase
  for (char& c : ext) c = tolower(c);
  return ext == ".pdf";
}

std::unique_ptr<Pdf> ReaderActivity::loadPdf(const std::string& path) {
  return Pdf::open(path);
}

void ReaderActivity::onGoToPdfReader(std::unique_ptr<Pdf> pdf) {
  currentBookPath = pdf->filePath();
  activityManager.replaceActivity(
    std::make_unique<PdfReaderActivity>(renderer, mappedInput, std::move(pdf)));
}
```

- [ ] **9.3 Add PDF branch to `onEnter()`** (where EPUB/TXT/XTC are dispatched)

```cpp
if (isPdfFile(initialBookPath)) {
  auto pdf = loadPdf(initialBookPath);
  if (!pdf) {
    onGoBack();
    return;
  }
  onGoToPdfReader(std::move(pdf));
  return;
}
```

- [ ] **9.4 Build and verify**

```bash
pio run -e default 2>&1 | tail -5
```

- [ ] **9.5 Commit**

```bash
git add src/activities/reader/ReaderActivity.h src/activities/reader/ReaderActivity.cpp
git commit -m "feat(pdf): wire PDF routing in ReaderActivity"
```

> **Device test checkpoint:** Flash firmware, copy a simple text PDF to SD, open it from the file browser. Verify it opens, displays reflowed text, and page turns work. Check serial for any LOG_ERR messages.

---

## Task 10: Image rendering in PdfReaderActivity

**Goal:** Decode and render embedded JPEG/PNG images inline between text blocks.

**Files:**
- Modify: `src/activities/reader/PdfReaderActivity.cpp`

- [ ] **10.1 Add image rendering to `renderContents()`**

After rendering each text block, check if any images fall between the current text position and the next block (use `orderHint` for ordering). For each image:

```cpp
for (const auto& imgDesc : page.images) {
  // 1. Open the PDF file (Pdf exposes a method or we reopen it)
  // 2. Extract image bytes at imgDesc.pdfStreamOffset, imgDesc.pdfStreamLength
  // 3. If JPEG (format==0): pass to JPEGDEC -> drawBitmap
  // 4. If PNG (format==1): decompress via StreamDecoder -> pass to PNGdec -> drawBitmap
  // 5. Scale to fit (maxWidth = orientedViewableWidth)
  // 6. On failure: draw "[image]" placeholder text
}
```

- [ ] **10.2 Expose file access from Pdf class**

Add a method to `Pdf.h`:
```cpp
// Extract raw bytes of an image stream into caller-provided buffer.
// Returns actual bytes written, or 0 on failure.
size_t extractImageStream(const PdfImageDescriptor& img,
                          uint8_t* outBuf, size_t maxBytes);
```

Implement in `Pdf.cpp` using `impl->file` (already open).

- [ ] **10.3 Build and verify**

```bash
pio run -e default 2>&1 | tail -5
```

- [ ] **10.4 Commit**

```bash
git add src/activities/reader/PdfReaderActivity.cpp lib/Pdf/Pdf.h lib/Pdf/Pdf.cpp
git commit -m "feat(pdf): add inline image decoding and rendering"
```

> **Device test checkpoint:** Open a PDF with embedded JPEG images. Verify images render, text surrounds them. Check heap usage via `ESP.getFreeHeap()` logs.

---

## Task 11: PdfReaderChapterSelectionActivity (TOC)

**Goal:** Add a TOC/outline navigator, launched from the reader menu.

**Files:**
- Create: `src/activities/reader/PdfReaderChapterSelectionActivity.h`
- Create: `src/activities/reader/PdfReaderChapterSelectionActivity.cpp`
- Modify: `src/activities/reader/PdfReaderActivity.cpp` (add menu button handler)

- [ ] **11.1 Create `PdfReaderChapterSelectionActivity.h`**

Mirror `EpubReaderChapterSelectionActivity.h`. The activity receives the outline list and a callback to jump to a page.

```cpp
#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include <Pdf.h>
// Note: do NOT include <functional> — std::function is prohibited (adds ~2-4KB heap allocation per signature)

class PdfReaderChapterSelectionActivity final : public Activity {
  // Owned copy (not a reference) so this activity can outlive the Pdf object without dangling
  std::vector<PdfOutlineEntry> outline;

  // Plain function pointer pattern (no std::function — prohibited by project rules)
  struct PageSelectedCallback { void* ctx; void (*fn)(void*, uint32_t pageNum); };
  PageSelectedCallback onPageSelected = {nullptr, nullptr};

  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;

 public:
  // Constructor takes the outline by const-ref and copies it — safe even if Pdf is destroyed
  explicit PdfReaderChapterSelectionActivity(GfxRenderer& renderer,
                                             MappedInputManager& mappedInput,
                                             const std::vector<PdfOutlineEntry>& outline,
                                             PageSelectedCallback onPageSelected)
      : Activity("PdfChapterSelection", renderer, mappedInput),
        outline(outline),
        onPageSelected(onPageSelected) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
```

- [ ] **11.2 Implement chapter selection activity**

Follow `EpubReaderChapterSelectionActivity.cpp` exactly. Display list of `entry.title + " p." + entry.pageNum`. On confirm: call `onPageSelected(entry.pageNum)`.

- [ ] **11.3 Add menu to `PdfReaderActivity`**

On `Button::Confirm` (long press or menu button, check EPUB pattern): push a simple menu with [Outline] and [Home]. On [Outline]: push `PdfReaderChapterSelectionActivity`.

- [ ] **11.4 Build and verify**

```bash
pio run -e default 2>&1 | tail -5
```

- [ ] **11.5 Commit**

```bash
git add src/activities/reader/PdfReaderChapterSelectionActivity.h \
        src/activities/reader/PdfReaderChapterSelectionActivity.cpp \
        src/activities/reader/PdfReaderActivity.cpp
git commit -m "feat(pdf): add outline/TOC navigator activity"
```

---

## Task 12: I18n strings + error handling polish

**Goal:** Add all user-facing strings to the i18n system and harden error paths.

**Files:**
- Modify: `lib/I18n/translations/english.yaml`
- Run: `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/`

- [ ] **12.1 Add strings to `english.yaml`**

```yaml
STR_PDF_LOAD_ERROR: "Failed to open PDF"
STR_PDF_IMAGE_PLACEHOLDER: "[image]"
STR_PDF_PAGE_OF: "Page %d / %d"
STR_PDF_OUTLINE_EMPTY: "No outline available"
```

- [ ] **12.2 Regenerate I18n files**

```bash
python scripts/gen_i18n.py lib/I18n/translations lib/I18n/
```

- [ ] **12.3 Replace hardcoded strings in PdfReaderActivity with `tr()` calls**

Search for any `"Failed"`, `"[image]"`, `"Page"` string literals and replace with `tr(STR_...)`.

- [ ] **12.4 Build and verify**

```bash
pio run -e default 2>&1 | tail -5
```

- [ ] **12.5 Commit**

```bash
git add lib/I18n/translations/english.yaml src/activities/reader/PdfReaderActivity.cpp \
        src/activities/reader/PdfReaderChapterSelectionActivity.cpp
git commit -m "feat(pdf): add i18n strings and harden error handling"
```

---

## Task 13: Final integration test and cleanup

- [ ] **13.1 Run static analysis**

```bash
pio check 2>&1 | grep -E "error|warning" | head -20
```

Fix any warnings in new PDF files.

- [ ] **13.2 Run clang-format**

```bash
find lib/Pdf src/activities/reader/PdfReader* -name "*.cpp" -o -name "*.h" | xargs clang-format -i
```

- [ ] **13.3 Device test checklist (manual, for human tester)**

```
[ ] Open a text-only PDF — pages display correctly
[ ] Open a PDF with JPEG images — images render inline
[ ] Open a PDF with an outline — TOC shows chapter list, tap navigates to page
[ ] Close and reopen same PDF — opens instantly from cache at saved page
[ ] Check serial: no LOG_ERR during normal reading
[ ] Check heap: ESP.getFreeHeap() > 50KB throughout reading
[ ] Orientation change — layout re-renders correctly
[ ] File browser shows .pdf files in file list
```

- [ ] **13.4 Final commit**

```bash
git add -p  # review all remaining changes
git commit -m "feat(pdf): complete basic PDF reader with text reflow, images, and TOC"
```

---

## Summary

| Task | Component | Outcome |
|------|-----------|---------|
| 1 | Skeleton | All files in place, builds |
| 2 | XrefTable | PDF object lookup works |
| 3 | PdfObject | Raw dict parsing works |
| 4 | PageTree + StreamDecoder | Page location + FlateDecode |
| 5 | ContentStream | Text + image extraction |
| 6 | PdfOutline + PdfCache | Bookmarks + SD cache |
| 7 | Pdf.cpp | End-to-end open + getPage |
| 8 | PdfReaderActivity | Text rendering on device |
| 9 | ReaderActivity routing | Opens from file browser |
| 10 | Image rendering | Inline images work |
| 11 | TOC navigator | Outline browsing works |
| 12 | I18n + error handling | Production-quality polish |
| 13 | Cleanup + test | Ready to ship |

# PDF reading support

## Entry point

`Pdf::open(path)` (`lib/Pdf/Pdf.cpp`) opens the file via `HalStorage`, parses the cross-reference data, loads the catalog and page tree, configures the on-device cache, and loads outline metadata from cache when possible. If the cached outline does not match the parsed page count, it rebuilds the outline from the file and saves it back to cache.

## Cross-reference (`lib/Pdf/Pdf/XrefTable.*`)

The xref tells us where each object starts in the file, or that it lives inside an **object stream**.

1. **Classic tables** — Lines starting with `xref`, subsection rows, and a `trailer` dict. Incremental PDFs chain older sections through `/Prev`; sections are merged **oldest first**, then newer updates overwrite. Empty subsections (`0 0`) are allowed.

2. **XRef streams** — When `startxref` points at an object whose dict has `/Type /XRef`, the xref is read from a **FlateDecode** stream. Rows use `/W` field widths: type **1** is a normal file offset; type **2** means the object is stored in an **object stream** (`/Type /ObjStm`) identified by another object number. `/Index` is handled as a sequence of (start,count) ranges.

3. **Object streams** — For each referenced ObjStm object, the library decompresses the stream, parses the header integers (`/N`, `/First`), and splits embedded objects. Offsets in the header are **relative to `/First`** in the decoded stream. Parsed entries are cached in an inline object cache (`inline_`) with per-object dict bytes and optional stream bytes for content objects.
   - On `HAL_STORAGE_STUB` builds, the decoded stream bytes are held in memory for extraction.
   - On normal HAL storage builds, the object stream is temporarily decoded to `/.crosspoint/pdf/objstm_*.tmp` before parsing.

4. **Reading objects** — `readDictForObject` resolves a dict from a file offset or from the object-stream cache. `readStreamForObject` returns dict + payload bytes for stream objects (file-backed or cached). Page tree walking and outlines use dict reads; page **content** uses stream reads.

## PDF objects (`lib/Pdf/Pdf/PdfObject.*`)

`readAt` skips the `id gen obj` header and returns the dictionary body, or locates `stream` / `endstream` for stream objects. **`/Length` may be indirect** (`n g R`); when an `XrefTable*` is passed in, the length is read from that object’s stream (not from the first integer of the reference alone).

## Page tree and navigation (`lib/Pdf/Pdf/PageTree.*`)

Starting from `/Pages` in the catalog, the code walks `/Kids` arrays, treating `/Pages` nodes as internal and `/Page` nodes as leaves. Each page stores both a **file offset** (may be zero for object-stream-only objects) and the **object id**; `Pdf::getPage` uses the object id plus `readDictForObject` / `readStreamForObject`.

## Outlines (`lib/Pdf/Pdf/PdfOutline.*`)

Optional `/Outlines` tree is walked using `readDictForObject`. Titles and destinations are interpreted enough to map many entries to a page index.

## Content streams (`lib/Pdf/Pdf/ContentStream.*`, `StreamDecoder.*`)

Page `/Contents` (and similar) stream bytes are optionally **FlateDecode**d (`StreamDecoder`, uzlib-backed `InflateReader`). Operators are scanned in a small subset: text placement and emission (`BT`/`ET`, `Tf`, `Tm`, `Td`, `TD`, `T*`, `Tj`, `TJ`, `'`, `"`), and image `Do` via `/Resources` → `/XObject` (file-offset images only). Output is a `PdfPage`: text blocks in rough reading order, image descriptors, and a simple draw order list.

**Text extraction** is intentionally limited: string literals and hex strings are mapped with a WinAnsi-style table and UTF-16 BOM handling, plus partial `/ToUnicode` CMap support for common Identity-H fonts. **CID fonts and glyph-index `TJ` arrays** are still not fully interpreted, so some PDFs show little or no text even when the pipeline succeeds.

## Cache (`lib/Pdf/Pdf/PdfCache.*`)

Rendered `PdfPage` data, outline metadata, and progress state are cached under `/.crosspoint/pdf_<hash>/` so reopening the same file can skip most parsing work.

## Host tests (`test/pdf/`)

`run_pdf_parser_tests.sh` builds `test/pdf/build/pdf_parser_host_test` against the same `lib/Pdf` sources with a tiny in-memory `HalFile` stub, to validate xref, page tree, outlines, and content parsing on sample files without flashing firmware.

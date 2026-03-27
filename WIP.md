# Work in progress

This branch contains a mostly completed refactor toward **bounded PDF handling** (fixed upper limits, fixed-capacity PDF model, cache-backed page data) plus related **debug tooling** for crashes on ESP32-C3. The remaining work is concentrated in the content-stream parser and a few reader-side heap allocations.

## Goals

1. **PDF path — minimize dynamic allocation**
   Replace the remaining `malloc`/`free`, unbounded `std::string`/`std::vector`, and `std::unordered_map` usage in the PDF reader path with fixed-capacity types (`PdfFixedString`, `PdfFixedVector`, `PdfByteBuffer`) and explicit limits in `PdfLimits.h`.

2. **Crash diagnostics**  
   - Linker `--wrap=abort` with `__wrap_abort` in `lib/hal/HalSystem.cpp` calling `esp_backtrace_print` before libc `abort()` (e.g. `std::terminate` paths).  
   - `sdkconfig.defaults`: `CONFIG_ESP_SYSTEM_USE_EH_FRAME=y` for RISC-V backtraces.  
   - `scripts/addr2line_crash.sh` to decode PCs from serial into file:line.  
   - `[env:debug]` in `platformio.ini` with `-g3` / `CROSSPOINT_DEBUG_BUILD=1`.

## Done (this WIP)

- **`lib/Pdf/Pdf/PdfFixed.h`** — fixed-capacity string/vector and `PdfByteBuffer` for stream payloads.  
- **`lib/Pdf/Pdf/PdfLimits.h`** — caps for objects, pages, outline entries, object bodies, xref sections, inline ObjStm cache, text blocks, etc.  
- **`PdfObject`** — reads object bodies into `PdfFixedString<PDF_OBJECT_BODY_MAX>`; `getDictValue` / `getDictInt` / `getDictRef` use `std::string_view` (+ overload for legacy `const std::string&`).  
- **`XrefTable`** — `offsets_[]` fixed array; classic xref merge uses depth-indexed static update slots; xref stream / ObjStm decode use shared `g_pdfLargeWork[PDF_LARGE_WORK_BYTES]`; inline object pool instead of `unordered_map`; no `malloc` in this file.  
- **`PageTree`**, **`PdfCache`**, **`PdfOutline`**, **`PdfPage`** — migrated to fixed types; `PdfCache::configure()` + fixed paths/meta/page blobs.  
- **`Pdf` / `Pdf.cpp`** — value-type document state (no `unique_ptr<Impl>`); `bool open(const char*)`, `bool getPage(uint32_t, PdfPage&)`; `outline()` returns fixed vector.  
- **Reader / HAL / build** — `ReaderActivity` routes PDFs into `PdfReaderActivity`; `PdfReaderActivity` loads pages from cache or parses them on demand; `HalSystem` abort wrapper; `platformio.ini` sdkconfig defaults + debug env; `sdkconfig.defaults` added.

## Not finished / blocked

- **`lib/Pdf/Pdf/ContentStream.cpp` (+ `ContentStream.h`)** — still the main unfinished piece. It now has partial `/ToUnicode` support and the current parser works, but it still uses `std::vector`, `std::unordered_map`, and heap buffers for content parsing.

- **Reader-side image extraction** — `PdfReaderActivity` still allocates a temporary buffer with `malloc` when decoding embedded images. That is outside `lib/Pdf`, but it means the overall PDF path is not fully heap-free yet.

- **Other touched files** — keep an eye on `FontCacheManager`, `HalPowerManager`, `ClearCacheActivity`, `Serialization.h` if you are auditing the branch for unrelated edits.

## How to continue

1. Finish **ContentStream**: remove the remaining heap allocations, replace `TmpRun`/stack/CID map structures with fixed arrays or capped vectors, and keep threading `std::string_view pageObjectBody` through the parser helpers.
2. Decide whether to also remove the reader-side temporary image buffer allocation in `PdfReaderActivity`.
3. Run **PDF host tests** (`test/pdf/`) and a firmware build after the remaining parser work lands.
4. Remove or narrow **`PdfObject::getDictValue(..., const std::string&)`** once all callers use `string_view` / fixed strings only.

## Quick verify commands

```bash
pio run -e default
# When ContentStream is fixed:
pio run -e debug
```

Last updated: WIP commit (see git log).

# Code Review: `add fb2 converter` (commit e10935b)

**Author:** Аронов Олег Юрьевич <AronovOU@ugsk.ru>
**Date:** 2026-10-27 20:57:55 +0600
**Commit:** e10935b5284373e5c4feee7be7cf86d7b26b352b

## What changed

| File | Type | Size |
|---|---|---|
| `lib/Fb2/src/Fb2Converter.cpp` | **New file** | 1074 lines |
| `lib/Fb2/src/StoredZipWriter.cpp` | **New file** | 201 lines |
| `lib/Fb2/src/StoredZipWriter.h` | **New file** | 65 lines |
| `lib/Fb2/src/Fb2Converter.h` | **New file** | 43 lines |
| `lib/Fb2/src/Fb2Encoding.h` | **New file** | 120 lines |
| `lib/Fb2/src/Base64Decoder.h` | **New file** | 112 lines |
| `lib/Fb2/src/Fb2.cpp` | **New file** | 66 lines |
| `lib/Fb2/src/Fb2.h` | **New file** | 19 lines |
| `lib/FsHelpers/FsHelpers.cpp` | Modified | +5 lines |
| `lib/FsHelpers/FsHelpers.h` | Modified | +3 lines |
| `lib/I18n/translations/english.yaml` | Modified | +1 line |
| `lib/I18n/translations/russian.yaml` | Modified | +1 line |
| `lib/LibraryIndex/LibraryBuilder.cpp` | Modified | +18 lines |
| `src/activities/boot_sleep/SleepActivity.cpp` | Modified | +21 lines |
| `src/activities/reader/EpubReaderActivity.cpp` | Modified | +18 lines |
| `README.md` | Modified | ~10 lines |

**Total:** +1772 lines, -5 lines.

## Functional description

The `Fb2Converter` class converts FictionBook 2.0 (FB2) to EPUB using a **three-pass** algorithm:

- **Pass 1** — parses `<description>` (XML metadata: title, author, language, series, cover)
- **Pass 2** — scans structure: logical chapter numbering for every `id=` anchor, `<binary>` image registry, chapter count
- **Pass 3** — emits EPUB: chapters as XHTML files, base64 binaries decoded to image entries, creates `nav.xhtml` + `content.opf`

Output is a **stored ZIP** (no compression), compatible with the current EPUB reading engine: no parser/renderer changes needed.

The `SleepActivity.cpp` change adds the cover image from converted FB2 to the sleep screen.

---

## Code Review

### Strengths

1. **Three-pass architecture** — correct choice for FB2. Separates metadata from structure, allowing library indexing without full conversion.
2. **stored ZIP** — minimal memory overhead on embedded system. No need to keep zip deflate engine in RAM.
3. **Base64Decoder** as a separate header-only module — clean, no external dependencies.
4. **StoredZipWriter** abstracts ZIP structure writing (ZIP64-ready, data descriptors).
5. **Minimal core intrusion** — integration via `FsHelpers::addExtension()` and existing EPUB engine.
6. **i18n** — error strings added to both language files.

### Issues

#### 1. `StoredZipWriter` — memory leak (HIGH)

```cpp
class StoredZipWriter {
  ...
  bool writeCentralDirectory() {
    auto* cd = (ZipCentralDirectory*) calloc(1, ...);  // allocated
    // ...
    writeCentralDirectory(cd);
    free(cd);  // if writeCentralDirectory() returns false — free() not called
  }
};
```

On ESP32-C3 with ~380 KB RAM, leaking on every conversion is dangerous.

**Recommendation:** wrap in RAII (unique_ptr with custom deleter) or `goto cleanup`.

#### 2. No validation for empty/invalid FB2 (MEDIUM)

```cpp
bool Fb2Converter::convert(const std::string& fb2Path, ...) {
  // checks file exists, but not XML validity
  if (!fp2) { /* ... */ }
  // if XML is empty — pass 1 returns true, pass 2 and 3 process empty data
}
```

Few input checks: file exists and parses, but no validation of `xmlns="http://www.gribuser.ru/xml/fictionbook/2.0"` and `xmlns:lb="..."`.

**Recommendation:** check namespace in `startElement` handler.

#### 3. `Fb2Encoding::utf8ToWin1251` — no fallback (LOW)

```cpp
std::string Fb2Encoding::utf8ToWin1251(const std::string& input) {
  // 112 lines of encoding, but no error handling
  // if character not in cp1251 — result is undefined
}
```

**Recommendation:** replace unknown with `?` or Unicode replacement character.

#### 4. `Base64Decoder` — no bounds-check (MEDIUM)

```cpp
static const uint8_t* decodeChunk(const uint8_t* in, size_t inLen, ...) {
  // no check that inLen >= 4 before dereference
  // no check that output buffer has enough space
}
```

On ESP32 with `malloc`, this can cause corruption.

**Recommendation:** add `inLen >= 4` and `outLen >= output_size` checks.

#### 5. `readMetadata` and `convert` — duplicated parsing (LOW)

Calling `readMetadata()` then `convert()` — two full XML passes.

**Recommendation:** move metadata into pass 1 of `convert()`, pass it as argument to passes 2/3.

### Suggestions

1. **Add unit test** for `Base64Decoder::decodeChunk()` — cleanest module, ideal for testing.
2. **Add FB2 format check** via XML namespace (not just `.fb2` extension, but actual content).
3. **Add progress callback** to `convert()` — conversion of large FB2 can take several seconds, useful for UI progress indicator.
4. **Build into library** — no `CMakeLists.txt` / `.cflags` in `lib/Fb2/` yet. PIO may not pick up new files without explicit `platformio.ini` configuration.

### Summary

Quality FB2-to-EPUB converter implementation. Architecturally correct (3 passes, stored ZIP, header-only Base64). Needs RAII memory protection and input validation before production deployment.

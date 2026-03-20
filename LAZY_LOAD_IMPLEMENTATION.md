# Lazy-Load EPUB Implementation - Phase 1

## Quick Analysis

Current flow in `Epub::load()`:
1. Initialize BookMetadataCache
2. Try to load existing cache
3. If no cache, parse content.opf (metadata + cover reference)
4. Parse CSS files
5. (Somewhere) Try to generate cover BMP from 1.1MB image → **HANGS HERE**

## Phase 1: Skip Cover Generation on Initial Load

### Strategy
Don't generate cover BMP until explicitly requested. This will:
1. Speed up initial load from 5-10s to <1s
2. Avoid allocating 1.1MB for cover image
3. Defer cover rendering until user explicitly views it

### Implementation (Small, Focused Change)

**File: `lib/Epub/Epub.cpp`**

In `Epub::load()`, add parameter to skip cover generation:

```cpp
bool Epub::load(const bool buildIfMissing, const bool skipLoadingCss, const bool skipCoverGen = true) {
  LOG_DBG("EBP", "Loading ePub: %s", filepath.c_str());
  
  // ... existing cache/css loading code ...
  
  // NEW: Skip cover generation if requested (default true for initial load)
  if (!skipCoverGen) {
    // Generate cover BMP only if explicitly requested
    generateCoverBmp();
  }
  
  return true;
}
```

**File: `src/activities/reader/EpubReaderActivity.cpp`**

When opening EPUB for reading:

```cpp
// Current code (generates cover immediately)
auto epub = std::unique_ptr<Epub>(new Epub(path, "/.crosspoint"));
epub->load();  // This hangs on large covers

// New code (skips cover, shows content immediately)
auto epub = std::unique_ptr<Epub>(new Epub(path, "/.crosspoint"));
epub->load(true, false, true);  // Skip cover generation
renderFirstPage();  // Show content immediately
// Cover loads later when/if user views it
```

**File: `src/activities/reader/EpubReaderActivity.cpp` or UI code**

When user requests cover display:

```cpp
// Generate cover on-demand (only when viewed)
if (!epub->getCoverBmpPath().empty()) {
  epub->generateCoverBmp();  // Will be fast if cache exists
}
```

### Expected Impact

| Metric | Before | After |
|--------|--------|-------|
| Initial load | 5-10s | <1s |
| Memory spike | 1.1MB+ | <50KB |
| First page display | After 5-10s | Immediate (~1s) |
| Cover generation | On load | On-demand |
| User experience | Frustrating hang | Smooth, responsive |

### Files to Modify

1. `lib/Epub/Epub.h` - Add parameter to load()
2. `lib/Epub/Epub.cpp` - Conditional cover generation
3. `src/activities/reader/EpubReaderActivity.cpp` - Call with skip flag
4. Any cover display code - Generate on-demand

### Testing

1. Open Omniversal EPUB
   - Should show first page within 1 second
   - No hang or freeze
   - Memory stays below 50KB

2. Open normal EPUBs (<500KB)
   - No regression
   - Same load behavior as before

3. View cover
   - First time: generates BMP (slow but no hang)
   - Subsequent: loads from cache (fast)

### Notes

- This is a **minimal change** (3-4 lines of code)
- No refactoring needed
- Backward compatible
- Can be extended to lazy-load chapters in Phase 2

### Next Steps (After Testing)

**Phase 2**: Lazy-load chapters
- Don't pre-build all section caches
- Build on first access
- Reduces initial metadata parsing time

**Phase 3**: Streaming sections
- Stream chapter content directly from ZIP
- Only cache current + next chapter
- Maximum memory efficiency

# Omniversal EPUB Hang Issue

## Problem
Large EPUB files (>1MB with large cover images) hang on load and don't render.

### Root Cause
- **File size**: my-omniversal-journeys.epub is 1.2MB
- **Cover image**: images/cover.png is 1.1MB (uncompressed in EPUB during parsing)
- **Memory constraint**: ESP32-C3 with ~91KB free heap cannot allocate 1.1MB
- **Blocking operation**: Current implementation tries to fully decode/process the entire EPUB structure and cover image before showing the first page
- **No streaming**: The parser loads and processes all metadata before rendering anything

## Current Behavior
1. User opens Omniversal EPUB
2. ReaderActivity calls `new Epub(path)` 
3. Epub constructor begins parsing
4. Parser loads entire metadata structure + cover image
5. Tries to allocate >1.1MB for cover
6. Heap exhausted → hang or crash
7. Reader never shows first page

## Solution: Lazy Loading & Page-First Rendering

### Implementation Plan

**Phase 1: Render First Page Immediately**
- Parse only minimal metadata needed to render first chapter
- Skip cover image decoding during initial load
- Render first page content ASAP (within 1-2 seconds)
- Parse remaining chapters in background as user reads

**Phase 2: On-Demand Cover & Metadata**
- Load cover image only when explicitly requested (settings, home screen)
- Parse chapter metadata lazily per chapter
- Build toc.ncx index on-demand

**Phase 3: Memory-Efficient Streaming**
- Process EPUB entries one at a time
- Don't hold entire ZIP in memory
- Stream chapter HTML directly to renderer
- Cache only current + next chapter

### Code Changes Required

#### 1. Epub Constructor - Split Responsibility
```cpp
// Current: Constructor does everything
Epub::Epub(const std::string& path, const std::string& cache) { ... }

// New: Constructor only does minimal initialization
Epub::Epub(const std::string& path, const std::string& cache) {
  // Minimal: open ZIP, read container.xml, find first chapter
  // Skip: cover image, full metadata parsing, toc processing
}

// New method: Load full metadata (called later if needed)
void Epub::loadFullMetadata() { ... }
```

#### 2. ContentOpfParser - Early Exit Option
```cpp
// Add parameter to parse only minimal info
void parseMetadata(bool fullParse = false) {
  if (!fullParse) {
    // Parse only: spine order, first chapter href
    // Skip: images, cover, all non-essential items
    return;
  }
  // Full parsing as before
}
```

#### 3. Section Cache - Lazy Load
```cpp
// Don't pre-cache all sections
// Load on first access:
Section* getSection(int index) {
  if (!sectionCache[index]) {
    sectionCache[index] = new Section(...);
  }
  return sectionCache[index];
}
```

#### 4. ReaderActivity - First Page Display
```cpp
// Show first page immediately
void EpubReaderActivity::initializeReader() {
  epub = new Epub(path, cache);  // Fast: minimal load
  renderFirstPage();              // Show content immediately
  
  // Background: load full metadata, cover, etc.
  // scheduleBackgroundParsing();
}
```

### Performance Impact

**Before (Current)**
- Open large EPUB → 5-10 second hang → shows first page

**After (Lazy Loading)**
- Open large EPUB → 1-2 second load → shows first page immediately
- Full metadata loads in background
- Cover loads on-demand

### Files to Modify

1. `lib/Epub/Epub.cpp` - Split constructor/initialization
2. `lib/Epub/Epub.h` - Add lazy-load methods
3. `lib/Epub/parsers/ContentOpfParser.h/cpp` - Add minimal-parse option
4. `src/activities/reader/EpubReaderActivity.cpp` - Display first page early
5. `src/activities/reader/Section.cpp` - On-demand loading

### Testing

1. **Omniversal EPUB** (1.2MB with 1.1MB cover)
   - Open book → should show first page in <2s
   - No hang or freeze

2. **Normal EPUBs** (<500KB)
   - No performance regression
   - Same load behavior as before

3. **Memory**
   - Verify heap doesn't exceed 50KB during load
   - Monitor background parsing memory usage

### Priority
**HIGH** - This blocks reading of large user-created content (story collections, reference materials)

## Notes
- Don't try to decode the cover image during initial EPUB load
- Stream chapter content directly from ZIP
- Only cache what's needed for current/next chapter
- Use background task/timer for full metadata parsing if available

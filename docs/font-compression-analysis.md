# Font Compression

## Overview

Reader font bitmaps are stored as DEFLATE-compressed groups in flash. UI fonts remain uncompressed for maximum responsiveness. A runtime `FontDecompressor` with a 4-slot LRU cache decompresses groups on demand during text rendering.

**Flash usage:**

| | Size | Flash Usage |
|--|------|-------------|
| Before | 6,302,476 B | 96.2% |
| After | 4,739,538 B | 72.3% |
| **Saved** | **1,562,938 B (1.49 MB)** | **23.9%** |

---

## Architecture

### Build-Time Compression (`fontconvert.py --compress`)

Glyph bitmaps are grouped and compressed with raw DEFLATE (zlib level 9, `wbits=-15`):

- **Group 0 (base):** All glyphs with codepoint <= 0x7E (ASCII printable range + control characters). Typically ~100 glyphs.
- **Groups 1+:** Remaining glyphs in sequential groups of 8.

Each group's bitmap data is concatenated and compressed independently. For compressed fonts, `EpdGlyph.dataOffset` becomes the byte offset *within the decompressed group* (not the global bitmap array).

### Data Structures (`EpdFontData.h`)

```c
typedef struct {
  uint32_t compressedOffset;   // Byte offset into compressed data array
  uint32_t compressedSize;     // Compressed DEFLATE stream size
  uint32_t uncompressedSize;   // Decompressed size
  uint16_t glyphCount;         // Number of glyphs in this group
  uint16_t firstGlyphIndex;    // First glyph index in the global glyph array
} EpdFontGroup;

typedef struct {
  // ... existing fields ...
  bool is2Bit;
  const EpdFontGroup* groups;   // NULL for uncompressed fonts
  uint16_t groupCount;          // 0 for uncompressed fonts
} EpdFontData;
```

### Runtime Decompression (`FontDecompressor`)

- **4-slot LRU cache** holding decompressed group buffers (heap-allocated)
- **Shared `tinfl_decompressor`** (~11 KB) between font decompression and EPUB/ZIP decompression (they never overlap - both run sequentially under `renderingMutex`)
- **O(1) group index lookup:** `glyphIndex < baseSize ? 0 : 1 + (glyphIndex - baseSize) / 8`

**Typical memory usage:**
- Base group buffer: ~5-7 KB
- 2-3 small group buffers: ~700 B each
- `tinfl_decompressor`: ~11 KB
- **Total: ~19 KB**

### Integration Points

| Component | Change |
|-----------|--------|
| `GfxRenderer` | `getGlyphBitmap()` helper routes to decompressor or direct flash access |
| `ZipFile` | Borrows shared `tinfl_decompressor` via static setter, falls back to local allocation |
| `main.cpp` | Initializes `FontDecompressor`, wires to renderer and ZipFile |

---

## Font Categories

| Category | Fonts | Compressed | Rationale |
|----------|-------|------------|-----------|
| Reader (Bookerly) | 16 (4 sizes x 4 styles) | Yes | Large bitmaps, only used during page render |
| Reader (NotoSans 12-18pt) | 16 (4 sizes x 4 styles) | Yes | Same as above |
| Reader (OpenDyslexic) | 16 (4 sizes x 4 styles) | Yes | Same as above |
| UI (Ubuntu 10/12pt) | 4 (2 sizes x 2 styles) | No | Small, used everywhere for UI responsiveness |
| UI (NotoSans 8pt) | 1 | No | Small UI font |

---

## Compression Results

Compression ratios by font family (average across all sizes/styles):

| Font Family | Avg Original | Avg Compressed | Avg Ratio |
|-------------|-------------|----------------|-----------|
| Bookerly | ~78 KB | ~39 KB | ~51% |
| NotoSans | ~76 KB | ~39 KB | ~52% |
| OpenDyslexic | ~56 KB | ~29 KB | ~55% |

Larger font sizes compress better (more bitmap data per glyph).

---

## Verification

`verify_compression.py` performs round-trip verification on all compressed font headers:
1. Parses the C header to extract compressed bitmap data and group metadata
2. Decompresses each group with raw DEFLATE
3. Verifies decompressed size matches `uncompressedSize`
4. Verifies each glyph's `dataOffset + dataLength` falls within the decompressed buffer

This runs automatically as part of `convert-builtin-fonts.sh`.

---

## Key Files

| File | Role |
|------|------|
| `lib/EpdFont/EpdFontData.h` | `EpdFontGroup` struct, extended `EpdFontData` |
| `lib/EpdFont/FontDecompressor.h/.cpp` | Runtime decompressor with LRU cache |
| `lib/EpdFont/scripts/fontconvert.py` | `--compress` flag for build-time compression |
| `lib/EpdFont/scripts/verify_compression.py` | Round-trip verification |
| `lib/EpdFont/scripts/convert-builtin-fonts.sh` | Font generation with `--compress` for reader fonts |
| `lib/GfxRenderer/GfxRenderer.cpp` | `getGlyphBitmap()` transparent bitmap access |
| `lib/ZipFile/ZipFile.cpp` | Shared `tinfl_decompressor` integration |
| `src/main.cpp` | Initialization wiring |

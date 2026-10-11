# Flash Font Partition (FlashFontPartition)

The device's `spiffs` partition (`0x360000` = 3,538,944 bytes at flash address `0xc90000`, see `partitions.csv`) is repurposed as raw storage. It is not a SPIFFS filesystem — nothing mounts it. `FlashFontPartition` (`lib/hal/FlashFontPartition.h`) copies `.cpfont` files into it and memory-maps them, so an SD-card font's tables are read from flash instead of being loaded into SRAM.

**The tail of the partition is not the font cache's.** The last `LANG_RESERVED_BYTES` (64 KB) belong to `FlashLangPartition`, which keeps the decompressed UI language there. `FlashFontPartition::fontUsableSize()` returns the part below that slot, and every write is budgeted against it. 64 KB is the `esp_partition_mmap` alignment, so the language slot starts on a mappable boundary.

## Why flash storage for fonts

Loading an SD-card font the plain way reads its metric and kerning tables into heap arrays. For a large family the kern and interval tables alone can exceed 100 KB. A mapped font reads most of that straight from flash through the cache:

- glyph records, the kern class tables and the kern matrix are read in place;
- glyph bitmaps for a page prewarm are read from the mapping too, so the per-page glyph arena is not allocated (glyphs the prewarm missed still load from SD);
- only the interval table and the ligature pairs are copied to the heap, because they contain `uint32_t` fields that need natural alignment.

The copy happens on demand. `SdCardFontManager::loadFamily` writes a family into the partition the first time the reader loads it and the partition does not already hold it (`writeFamily`), then maps it. A transient load, such as the font preview, passes `FlashCachePolicy::ReadOnly` and reads the file from SD without touching the partition. The flash copy is only a cache: the `.cpfont` files on the SD card are the originals.

## Partition layout

```
[4 B]  Magic: "CPFC"
[1 B]  Entry count (1..MAX_ENTRIES=16)
[3 B]  Reserved (zero)
[16 × 48 B = 768 B]  Index entries (unused entries left erased)
--- HEADER_BYTES = 776 bytes total ---

[variable, 4-byte aligned]  Font data blobs
...
[last 64 KB]  FlashLangPartition's language slot
```

Each 48-byte index entry:

```
[32 B]  familyName — null-padded C string (e.g. "Literata")
[1 B]   pointSize — u8 (e.g. 16)
[3 B]   Padding
[4 B]   dataOffset — u32 LE, byte offset from partition start
[4 B]   dataSize — u32 LE
[4 B]   Reserved
```

The `(familyName, pointSize)` pair is the lookup key. `MAX_ENTRIES = 16` is enough for three font families × five sizes plus margin.

## Write API

Font files are written once per write session using three calls:

```cpp
FlashFontPartition::beginWrite("Literata");
for (uint8_t ptSize : {10, 12, 14, 16, 18}) {
    FlashFontPartition::appendFile(sdPath, "Literata", ptSize);
}
FlashFontPartition::finaliseWrite();
```

**`beginWrite(familyName)`** erases everything below the language slot (`esp_partition_erase_range` up to `fontUsableSize()`), then starts the write session. It never erases the language slot: erasing the whole partition would wipe the UI language every time a font family was cached. Erase is sector-granular (4 KB sectors); erasing the font region once is cheaper than per-sector erase management.

**`appendFile(sdPath, familyName, pointSize)`** reads the `.cpfont` file from SD in 4 KB chunks and writes it to flash via `esp_partition_write`. Data offsets are 4-byte aligned. A file that would run past the usable size is refused. The function records the entry in the write session but does not write the index yet.

**`finaliseWrite()`** writes the 8-byte header (magic + count) and the packed entry table to the start of the partition. Index entries are written in order of insertion.

Every `beginWrite` erases all font data, so the partition holds one family at a time. `writeFamily` writes every size of the family when they all fit (and fit in `MAX_ENTRIES`), and only the requested size otherwise.

`readIndex()` rejects an index whose data runs past the usable size. That is the migration case: a partition written before the language reservation existed may hold font data where the language now lives, and treating it as invalid makes the next load re-cache the family from the SD card.

## Mmap read API

```cpp
const uint8_t* ptr;
size_t sz;
if (FlashFontPartition::mmap("Literata", 16, &ptr, &sz)) {
    SdCardFont font;
    font.loadFromMmap(ptr, sz, sdPath);
    // keep the mapping for as long as the font is loaded
}
```

**`mmap(familyName, pointSize, outPtr, outSize)`** reads the index from flash, finds the matching entry, and calls `esp_partition_mmap`. The mapped region covers only the entry: from `dataOffset` rounded down to a 64 KB page (the MMU granularity; `esp_partition_mmap` does the rounding and adjusts the returned pointer) to `dataOffset + dataSize` rounded up. `outPtr` points to the start of the font data; `outSize` is the raw `.cpfont` file size.

Map only what the entry needs. On the ESP32-C3 the app's code, its rodata and every runtime mapping share one 8 MB window of 128 pages, and the app itself takes about 96 of them, so roughly 31 are left for the font with a language pack mapped. Mapping from the partition start, as this once did, needed the whole family below the entry too, and a family's largest sizes (Bitter 18 pt: 41 pages) could not be mapped at all. A single file larger than the free window, such as a CJK face, still cannot be mapped and loads from SD. Every 64 KB the app image grows takes one page away from this window.

Only one mmap handle is active at a time.

**`unmap()`** releases the mmap handle. After this call the pointer returned by `mmap` is invalid.

## How `loadFromMmap` differs from `load`

Both paths produce an `SdCardFont` in the same usable state. The difference is where data lives.

`SdCardFont::load(sdPath)` opens the file, reads every table into heap-allocated arrays, and closes the file. All metadata is heap-owned (`metadataOwned_ = true`, `mmapDataBase_ = nullptr`).

`SdCardFont::loadFromMmap(base, size, sdPath)` reads the same binary format from the mmap pointer. For each style it:

- **Copies** `fullIntervals` to heap — `EpdUnicodeInterval` contains `uint32_t` fields that need natural alignment, which flash cannot guarantee. Styles with identical interval tables share one copy.
- **Aliases** the kern class tables directly — `EpdKernClassEntry` is `__attribute__((packed))`, so byte-granular reads are safe. The pointers point into flash address space.
- **Copies** `ligaturePairs` to heap — `EpdLigaturePair` contains `uint32_t` fields; alignment required.
- Sets `mmapDataBase_ = base`, which the kern matrix, the glyph records and the prewarm's bitmap reads use.
- Sets `metadataOwned_ = true` so the destructor knows to `delete[]` the heap copies.

The glyph records are read in place as `EpdGlyph`. That is safe because the record is 16 bytes and an interval 12, so every style's glyph section stays 4-byte aligned; a change to the `.cpfont` record size has to keep that or read records with `memcpy`.

`unloadMetadata()` and `reloadMetadata()` are no-ops for a mapped font: the heap copies are small and the rest is in flash.

## Kern matrix access: mmap vs SD

When building the per-page mini kern matrix (`buildMiniKernMatrix`), the code branches on `mmapDataBase_`:

**Mmap fast path** — zero heap, zero SD I/O:
```cpp
const int8_t* matrixBase = reinterpret_cast<const int8_t*>(mmapDataBase_ + s.kernMatrixFileOffset);
const int8_t* srcRow = matrixBase + (oldL - 1) * rowBytes;
```

**SD chunked path** — 4 KB chunk buffer, forward sweep through file:
```cpp
std::unique_ptr<int8_t[]> chunkBuf(new (std::nothrow) int8_t[KERN_CHUNK_BYTES]);  // 4096 bytes
// seeks only when the needed row falls outside the current chunk
```
Falls back to per-row reads if the chunk buffer cannot be allocated.

## Query API

```cpp
FlashFontPartition::hasValidIndex()          // partition has been written at least once
FlashFontPartition::hasEntry("Literata", 16) // specific entry present
FlashFontPartition::hasFamilyComplete("Literata", sizes, count) // all sizes present
FlashFontPartition::isMapped()               // mmap currently active
FlashFontPartition::fontUsableSize()         // bytes below the language slot
```

These are read-only and do not require a write session or active mmap.

## Lifetime and mmap validity

The mmap pointer is valid from `mmap()` to `unmap()`, and a mapped font keeps reading through it: the kern class pointers, the kern matrix, the glyph records and the prewarm's bitmaps all point into the mapping. So the mapping stays active for as long as the font is loaded. `SdCardFontManager` unmaps only before it loads another family (`loadFamily`) and in `unloadAll`, after the fonts that use the mapping have been removed, and when `loadFromMmap` fails.

`sdPath` is stored in `filePath_`. Glyphs that the page prewarm did not cover are read from the `.cpfont` on the SD card through that path.

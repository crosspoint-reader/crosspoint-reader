# On-device TTF fonts with a persistent glyph store: design

Status (2026-10-13): approved. A step-back review replaced the earlier `SdCardFont`-based integration
with the direct integration in section 3.

- **Step 0 (device benchmark): done.** Its numbers are in Appendix A; the decisions they drove are in
  section 2.
- **Stage 1: done, in review as PR #468** (branch `refactor/glyph-provider`, plan
  `docs/superpowers/plans/2026-10-13-ttf-stage1-glyph-provider.md`).
- **Stage 2a: merged** (PR #469: the glyph store, audited). Stage 2b (rasterizer) is next; 2c-2d are not
  planned yet.
- **Where everything lives:** section 8, "Branches and files".
- **A book's embedded fonts (section 11):** Stage A, the book's body family drawn as that book's font, is built on
  branch `feat/book-fonts` (2026-10-17; slimmed and X4-tested 2026-10-18). Stage B, more than one family per book,
  is designed in 11.1; phase 1 is merged (11.1.1), phase 2, the store of a family set, is built (11.1.2).

It is working material. When the work merges, lasting parts move to reference docs, open items move
to `docs/future_work/`, and this file is deleted (see the docs layout convention).

## 1. Goal

A reader copies a `.ttf` family to the SD card and reads with it. There is no PC conversion step. The
target is the ESP32-C3 boards (X3/X4: no PSRAM, about 25-39 KB of largest free heap block while
reading). The same code runs on S3 boards.

Success means:

- Once a glyph is stored, a page turn with a TTF font is as fast as with a flash-mapped `.cpfont`.
- Rendered glyphs survive power cycles, so a new session does not render them again.
- Heap use is no worse than today's SD-font path.
- Latin families and CJK fonts both work. CJK fonts are 5-20 MB per weight.
- Headings and other CSS sizes snap to the reader's size ladder (10-26 pt, `FONT_SIZE_RUNGS`) and
  render natively at that rung, as built-in fonts do. Only the small remainder is resampled.

Out of scope, by decision: OTF/CFF outlines and any other format. The aim is TTF, done properly and fast.

## 2. Decisions and their evidence

All numbers were measured on 2026-10-13. The flash figures come from RISC-V cross-compiles at -Os
after garbage collection. The speed figures come from a host build and only rank the engines; device
numbers come from step 0.

| Decision | Why |
|---|---|
| No FreeType | 98 KB of flash, or 149 KB with the auto-hinter, against about 280 KB of headroom. |
| No stb_truetype, libschrift or bb_truetype | **stb:** a 56,004 B allocation per glyph on RV32 (`stbtt__hheap_alloc`), and edges are dropped silently when it fails. Its float maths is emulated. **libschrift:** a 262,880 B stack frame in `render_outline`, and `double` maths throughout. **bb_truetype:** 1-bit output, no bounds checks, kerning that does not compile. |
| Engine: our own integer TrueType loader plus FreeType's `ftgrays.c` built standalone | 6.4 KB of flash and **no soft-float routine linked**. Across 12,868 glyphs (all of Bookerly, Noto Sans, Inter and DejaVu Sans at 14 pt), compared with FreeType unhinted: no load failures, no cmap or advance differences, 5 box differences, and the 2-bit output differs on 2.1-2.8% of pixels. Host speed 4-9 us/glyph; FreeType took 6-40. |
| `ftgrays` takes FreeType's 64-bit path (`FT_INT64` defined in the standalone build); decided 2026-10-14 | Output byte-identical to FreeType for every glyph without overlap flags, and the same on host and device. The 32-bit path is 16% faster per glyph (363 against 313 µs for text glyphs at 14 pt on the X3), but it differs from FreeType by one gray level on one pixel in 2-3% of glyphs and depends on the width of `long`. Appendix A, "Stage 2b engine". |
| Render lazily and persist the result | Chosen over converting the whole font once at import, which would mean a long wait for CJK. |
| Glyph store in the `spiffs` partition (raw flash, memory-mapped) | Fast random reads with no heap; it survives power-off; it is already used for the `.cpfont` cache. |
| Wipe the store on family change | One family at a time, as with `.cpfont` today. |
| **Direct integration through the glyph-miss hook**, not through `SdCardFont` | `SdCardFont`'s per-page machinery works around SD reads: per-page lookup tables, a bitmap arena with a retry ladder, metrics-only layout, a per-page kern matrix, a heap miss ring. The mapped store makes all of it unnecessary, because a lookup returns a pointer into flash. Reusing that machinery would have meant two large refactors of a 2,000-line class on the page-turn path. |
| Render a glyph at its first lookup, in layout or in a draw | New glyphs are rendered during section builds, which already run in the background or behind the "Indexing" popup. Page turns then rarely render anything. There is no metrics-only mode to maintain. |
| Native ladder rungs, created lazily | Sharp headings are a main benefit of TTF. Rungs are the absolute `FONT_SIZE_RUNGS` (at most 9), so the store holds a bounded set of sizes. A rung costs about 0.5 KB of RAM. |
| A resident scratch block per loaded family, holding a 4 KB cell pool (8 KB for SD-mode families) | Section builds already borrow the lent framebuffer. With the pool on the stack, rendering took 4.1 / 6.1 / 10.2 KB of the loop task's 16 KB stack for a 2 / 4 / 8 KB pool (X3, step 0). The pool size is a speed dial: against 2 KB, 4 KB is 9-18% faster (Bookerly 29 px: 469 → 429 µs), and 8 KB makes CJK about 37% faster (1,327 → 833 µs). Output is byte-identical across pool sizes. |
| Kerning: pairs stored lazily in the store's pair index; a miss is resolved by a GPOS reader that pre-resolves the kern subtables once | Measured on the X3 over a real page (736 pairs) and 30,000 characters of prose (19,379 pairs), every option exact. FreeInkFont's `Gpos.cpp` re-parses GPOS on every call: 137 µs/pair. The pre-resolved reader: 37.7 µs. Tables compiled at install: 3.0 µs, but 1.4 s compile, 71 KB flash and a 114 KB heap peak per style. A RAM pair cache: 2.0 µs over a chapter, 5 KB RAM, cold every session. The lazy store: **0.45 µs warm**, no RAM, about 230 slot writes on a first page and about 400 more over a first chapter. The user chose the lazy store. |
| Misses are rendered in glyph-ID order, batched per text run | On a real 19.7 MB CJK font on the SD card, reading each glyph's outline costs 4.76 ms in text order and 0.73 ms in file order, because SdFat walks the FAT chain on every backward seek. Glyph IDs follow file order. |

## 3. Architecture

```
GfxRenderer ── fontMap ── EpdFontFamily ── EpdFont ── EpdFontData (one per size and style)
                                                         │ provider (new)
                                                         ▼
                                   GlyphProvider ── SdCardFont (existing contexts, unchanged behaviour)
                                                 └─ TtfFont (new, one per loaded TTF family)
                                                      ├─ FontBytes        reads font bytes: mapped flash, or SD through a page cache
                                                      ├─ TtfRasterizer    integer TrueType loader + standalone ftgrays
                                                      ├─ FlashGlyphStore  write-once index + append-only log in `spiffs`
                                                      │                   (flash through NorFlash; device backend EspNorFlash in lib/hal)
                                                      └─ GPOS reader, Gsub  kerning (pre-resolved subtables) and ligatures (no FreeType)
```

### 3.1 `GlyphProvider` (replaces `glyphMissHandler` and `glyphMissCtx` in `EpdFontData`)

Today `EpdFontData` carries a miss callback plus a `void*` context. `GfxRenderer::getGlyphBitmap`
treats any non-null context as an `SdCardFont` (`SdCardFont::fromMissCtx`) to recognise its ring
glyphs, so a second provider would be misread. Those two fields are replaced by one interface pointer:

```cpp
class GlyphProvider {
 public:
  // A glyph the font's own tables do not have (all of them, for a TTF font).
  // The record lives at least until the next call on this provider.
  virtual const EpdGlyph* missGlyph(uint32_t codepoint) = 0;
  // True when the provider keeps this record's bitmap itself (ring glyphs); `bitmap` then gets it,
  // which is nullptr for a glyph with no pixels. False: the bitmap is at EpdFontData::bitmap +
  // dataOffset. A plain pointer return could not tell "mine, no pixels" from "not mine".
  virtual bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const { return false; }
  // 4.4 fixed-point pixels, like the kern tables. Asked only when the font has no kern tables.
  virtual int8_t kerning(uint32_t leftCp, uint32_t rightCp) { return 0; }
  virtual ~GlyphProvider() = default;
};
```

- **`EpdFont::findGlyph`** calls `provider->missGlyph` where it called the callback.
- **`EpdFont::getKerning`** asks the provider when no kern tables are set.
- **`GfxRenderer::getGlyphBitmap`** asks `provider->ownsBitmap` instead of the cast.
- **`SdCardFont`** implements the interface with the per-style contexts it already has. Its ring
  glyphs report their heap bitmap through `ownsBitmap`; its behaviour does not change.
- **Built-in fonts** leave the pointer null, so the generated font headers keep their initialisers.

### 3.2 `TtfFont` (lib/EpdFont/ttf/)

`TtfFont` is one object per loaded TTF family, and it implements `GlyphProvider`. It owns:

- `FontBytes`, the rasterizer, the scratch block, the store handle, and the ligature tables;
- **a face per size**. A face is just registration data: one `EpdFontData` per present style, and four
  `EpdFont` objects for the `EpdFontFamily` registered in `fontMap`.

Each face's `EpdFontData` holds:

- no intervals and no glyph table, so every lookup goes to the provider;
- `bitmap` = the mapped log base;
- `advanceY`, `ascender` and `descender` from that style's face metrics at that size (`hhea`, or OS/2's typo
  metrics when the font sets USE_TYPO_METRICS, as FreeType chooses them; `ttf::lineMetrics`);
- `is2Bit = true`;
- the style's ligature table;
- `provider` = a small per-(face, style) adapter that forwards to the `TtfFont` with its size and
  style.

A face costs about 0.5 KB: four `EpdFont`, four 116 B `EpdFontData`, and the adapters.

**A glyph lookup.** `missGlyph(cp)` turns the code point into a store key (code point, style, the
face's size index) and probes the mapped index.

- **Hit:** it returns a pointer to the `EpdGlyph` inside the log record, in flash. Its `dataOffset`
  is relative to the log base, so the renderer's normal bitmap path reads the bitmap in place, with no
  copy and no heap.
- **Miss:** it renders the glyph, stores it (4.5), and returns the stored record.
- **Ring mode:** used when the store is full, after a write error, or in previews (6, 7.6). The glyph
  is rendered into an 8-slot heap ring instead, and `ownsBitmap` reports the ring bitmap.

`TtfFont` is not registered in `sdCardFonts_` or `sdCardFontAliases_`. Those maps drive `SdCardFont`'s
own maintenance loops: metadata drop and restore, cache clear, prewarm, stats.

**Batching.** `GfxRenderer::ensureFontReady` (before layout measures a text run) and
`FontCacheManager::prewarmCache` (before a page is drawn) each gain a TTF branch, which calls
`TtfFont::prefetch(text, style)`.
- `prefetch` collects the codepoints of the text that are not stored yet, sorts them by glyph ID and
  renders and stores them in that order.
- Glyph IDs follow file order, so in SD mode the outline reads become forward reads (5.1).
- The per-glyph `missGlyph` path then finds them stored. It still renders anything the prefetch did not
  cover, one glyph at a time.

### 3.3 `GfxRenderer`: one lookup helper and a resolver

`GfxRenderer` looks fonts up with direct `fontMap.find` calls at more than 8 sites. Some draw paths
record text during the scan pass before any lookup; some measure paths look up first and return 0 on
a miss. A rung that does not exist yet would therefore be silently skipped by some paths. So:

- **One private lookup helper** replaces the direct calls.
- **On a miss, the helper calls an optional resolver** installed by `SdCardFontManager`. The resolver
  creates the face for a dormant TTF rung (5.5) and RETURNS the family; the helper registers it in
  `fontMap` (a plain emplace, not `insertFont()`). The resolver must not register it itself, and must
  not look fonts up through the renderer, which would recurse. (As built in Stage 1:
  `lib/GfxRenderer/FontLookup.h`, `GfxRenderer::setFontResolver`.)
- **A declined ID is asked again on every lookup**, so declining must be cheap.
- `fontMap` becomes `mutable`, the same compromise `sdCardFonts_` already makes.

With no resolver installed, or for an ID the resolver does not know, the behaviour is today's.

### 3.4 Concurrency

Layout runs on the loop task (background section builds) and drawing runs on the render task.
Both hold `RenderLock` around their work, and `SdCardFont`'s unprotected per-page state already
relies on that.

`TtfFont` relies on the same contract. Its miss path, prefetch, kerning store writes, scratch block, ring
and SD page cache are touched only under `RenderLock`. A debug-build assert on the miss path checks that the
calling task holds the rendering mutex.

Store hits read immutable flash. The write order (record first, slot last) means a probe never sees a
slot whose record is incomplete.

## 4. The glyph store (`FlashGlyphStore`, lib/EpdFont/ttf)

The store knows `EpdGlyph`, so it lives with the TTF code. It reaches flash only through `NorFlash`
(`lib/hal/NorFlash.h`): read, program (1 → 0 only), erase 4 KB sectors, map, free mapping pages.
`EspNorFlash` (`lib/hal`) implements it over the `spiffs` font area and is the only code that calls
`esp_partition_*`; host tests use a simulated NOR chip. Plan: `docs/superpowers/plans/
2026-10-14-ttf-stage2a-glyph-store.md`.

### 4.1 Partition layout in TTF mode

The `spiffs` partition holds either a `.cpfont` family (`FlashFontPartition`, magic `CPFC`) or one TTF
family (`FlashGlyphStore`, magic `CPTT`). `SdCardFontManager` decides which one to install. Both stay
below `FlashFontPartition::fontUsableSize()`; the last 64 KB is the language slot and is never touched.

```
sector 0 (4 KB)  header (4.2), written once per install
sector 1 (4 KB)  state sector: the size table (32 entries) and the wipe word; a wipe erases it last
                 (the rest of the first 64 KB page stays unused, so every region below starts on a page)
style data       flash mode: each style's whole TTF, every style 64 KB-aligned so it maps alone
                 SD mode: every table except glyf, per style (cmap loca hmtx head hhea maxp GPOS GSUB)
index            open-addressing hash table of 8-byte slots, erased (0xFF) at install
pair index       open-addressing table of 8-byte kerning slots (4.3), 64 KB, erased at install
log              append-only glyph records, erased at install
```

**Mode choice at install.** Flash mode applies if every style, plus the index and a minimum log of
256 KB, fits the usable partition and the mapping budget. Otherwise the font uses SD mode.

**Region sizes come from the mapping budget, not from the partition size.** The C3 has 128 MMU pages
of 64 KB shared by the app's code, its rodata and every mapping. About 31 are free at runtime, a
figure step 0 measures with `spi_flash_mmap_get_free_pages`. The index and the log stay mapped while
the font is loaded, so glyph records and bitmaps are read in place.

Rule at install:

```
log pages = free pages − (largest style, or the copied tables) − index pages − 1 (pair index) − 2 spare pages
```

The largest style is ≤10 pages in flash mode; the copied tables are about 7 pages for CJK in SD mode.

**The index is sized to the log:** enough whole pages that 70 % of its slots holds the records the log
holds, at about 100 B per record in flash mode and 250 B in SD mode. A fixed 8,192-slot index would
reach 70 % at about 5,700 Latin glyphs with the log only half full, and force a wipe (and its erases)
early. The planner (`glyphstore::planLayout`) shrinks the log until the index it needs also fits.

Examples from the planner at 32 free pages (step 0 measured 32):

| Mode | Also mapped | Index | Log (roughly) |
|---|---|---|---|
| Flash (Latin, 4 × 450 KB styles) | the style being rendered, one at a time | 128 KB (16,384 slots) | 17 pages ≈ 1.1 MB ≈ 11,000 glyphs at ~100 B (the partition is the limit) |
| Flash (4 × 629 KB styles) | the style being rendered | 128 KB (16,384 slots) | 9 pages ≈ 576 KB (the partition is the limit) |
| SD (CJK, one style, 7 pages of tables) | the copied tables | 64 KB (8,192 slots) | 21 pages ≈ 1.3 MB ≈ 5,500 glyphs at ~250 B |

The geometry is fixed at install from the budget measured then. If a later firmware update shrinks the
budget so that a mapping fails, the store reinstalls with smaller regions (6.2).

**The partition has one owner at a time.** `EspNorFlash` refuses writes while a `.cpfont` is mapped or
being written, and `FlashFontPartition` refuses to write or map while the glyph store holds a mapping.
Stage 2c closes the TTF store before it loads a `.cpfont` family.

### 4.2 Header (sector 0, written once per install) and the state sector (sector 1)

- magic `CPTT` and a format version;
- mode (flash or SD) and the style count;
- **family identity**: FNV-1a over each style file's name, byte size and FAT modification time, plus a
  render-version byte covering the engine, the 150 DPI sizing and the quantization thresholds;
- geometry: mode, style count, index offset and slots, pair index offset and slots, log offset and
  size;
- per style: the style block's offset and size; in SD mode also the SD path hash, the file size, and a
  table map (tag, offset in the file, offset in the style block, length; at most 8 tables, each inside
  its block) for each copied table;
- **a check**: FNV-1a over the header, written with it, so a header torn by a power cut reads as
  "not installed" and the family is installed again.

The state sector holds what changes after the install, so a wipe never rewrites the header:

- **size table**: 32 write-once entries `{u32 ppem 26.6, u32 claimed}`. An entry is in use once
  `claimed` reads 0. The ppem goes first and `claimed` last, so an entry cut short is neither free nor
  in use, and is never written again. 32 entries cover the 9 ladder rungs plus body sizes. (Stage B phase 2, 11.1.2:
  a face table, `{u32 family << 24 | ppem 26.6, u32 claimed}`, shared by a set's families);
- **wipe-pending word**: write-once, from 0xFFFFFFFF to 0. Anything but erased reads as pending.

**Size indices and wipes.** A wipe empties the size table. A commit refuses keys whose size index was not
claimed since the last wipe (`Commit::Refused`), and `generation()` changes with every wipe, so a face
that holds a size index claims it again when the generation moved.

### 4.3 Index slot: `{u32 key, u32 value}`

- `key = cp (bits 0-20) | style << 21 | sizeIndex << 23` (5 bits). Bits 28-31 are zero, so a key can
  never equal the erased value 0xFFFFFFFF.
- `value` is the record's `dataOffset` (its log offset + 20, never 0), or **0 = "the font has no glyph
  for this codepoint"**, or it can never load (6.1). Such slots have no log record, and the font is not
  asked again. A power cut can only leave extra 1 bits, so a torn slot never reads as 0; an earlier
  marker of 0xFFFFFFFE could be produced by a cut (audit, 2026-10-14).
- Slot position: `(uint64(key * 2654435761) * slots) >> 32`, which equals
  `(key * 2654435761) >> (32 - log2(slots))` for a power of two and allows any whole number of index
  pages; then linear probing up to 128 slots (every table has at least 128). A longer chain counts as
  full (4.6). A cap of 32 declared the index full at 49-67 % load in simulation.
- A slot is free only when both words read erased. A slot cut short (some bits programmed) is skipped by
  inserts and does not end a probe.
- **Pair slots** (the separate 64 KB pair index, 8,192 slots) are `{u32 leftCp << 16 | rightCp,
  u32 style << 16 | u16 value}`.
  - The value is the kerning in font units (int16), independent of size; it is scaled to 4.4 pixels
    per face at lookup.
  - A probe matches key and style.
  - Only BMP pairs are stored. Rarer pairs are resolved by the GPOS reader every time.
  - The pair index takes pairs up to 70 % of its slots; then pairs are no longer stored (never a wipe).

### 4.4 Log record

```
[u32 key][EpdGlyph, 16 B][2-bit bitmap, MSB-first stream], padded to 4 bytes
```

- **`EpdGlyph` sits at offset 4 of a 4-byte-aligned record,** so a pointer to it is a valid
  `const EpdGlyph*`, read from flash.
- **`dataOffset`** is relative to the log base.
- **The bitmap format matches `.cpfont` exactly:** 4 pixels per byte, 3 = full ink.
- **`dataLength` is capped at 8 KB.** A larger glyph cannot be stored, and the cap also bounds the
  recovery scan.

### 4.5 Writing, and surviving a power cut

NOR flash bits can only change from 1 to 0 without an erase, so:

1. program the records (key, `EpdGlyph`, bitmap). **They are batched:** `GlyphBatch` stages records in a
   caller buffer exactly as they sit in the log, and one program call writes the batch. Each call costs
   about 100 µs on top of its bytes (Appendix A), and each flash page is then programmed once;
2. program one index slot (8 bytes) per record.

- **Reads trust a slot only if** the record at `value − 20` lies inside the written log, has the same key,
  and is plausible (below). A record left behind by a power cut is never referenced, so it only wastes
  space.
- **At load the log is scanned record by record** to find where writing resumes. This takes a few
  thousand hops through mapped flash, well under 100 ms; checkpoints are deferred (section 11).
  - The walk stops at an erased key, or at a record that is not plausible: a key with bits 28-31 set,
    a `dataLength` that is not `glyphDataBytes(width, height, 2-bit)` or is over 8 KB, a `dataOffset`
    that is not the record's offset + 20, or a record past the log's end.
  - Whatever stopped it, a cut write can have left programmed bytes after that point, even behind a key
    that still reads erased. A torn write spans at most one record (8,212 B), so the scan looks that far.
    If it finds programmed bytes, it carries on at the first sector boundary past the stop that starts a
    plausible record (where a later session resumed), or else past those bytes at the next boundary,
    and walks again. A scan that only resumed once stopped at the same torn record on every later boot
    and wrote over what later sessions had stored (audit, 2026-10-14). No byte is ever programmed twice.
- **A slot cut short is skipped.** Insertion carries on probing past it.
- **ESP-IDF keeps the mapped view current.** It invalidates the mapped cache lines after every write
  (`spi_flash_check_and_flush_cache` in `memspi_host_driver.c`), so a record can be read back
  immediately.
- **Flash encryption is assumed off** for this partition, as it is now. Encrypted writes come in 16-byte
  blocks and would break write-once slots.

### 4.6 When the store fills

The store counts as full when any of these holds:

- the log is more than 95% used;
- the index is more than 70% loaded, or a probe chain passes 128 slots;
- all 32 size entries are taken and another size is asked for.

A full pair index only stops storing pairs (they come from the GPOS reader every time); it never causes
a wipe.

It then sets the wipe-pending word, and `TtfFont` switches to ring mode (3.2): glyphs are still drawn
but not stored. **The wipe itself runs only at a point where no `EpdGlyph` pointer into the log and no
size index can be in use: at book open, or before a section build starts.** Behind a "Preparing font"
popup it:

1. records wipe pending in flash and reads it back, if it was not pending already;
2. erases the index, the pair index and the log, only sectors that hold data;
3. erases the state sector last, which clears the size table and the wipe word together.

A cut anywhere leaves wipe pending, and the next wipe finishes the job; the header is never touched. A
full Latin store erases about 1.1 MB (2.2-2.7 s on the X3), a full CJK store about 1.3 MB.

**As built in 2c-2 (2026-10-15):**
- **A full size table stops only new sizes.** It makes the wipe pending, but the sizes already claimed go on storing;
  only the size the table refused is drawn from the ring until the wipe. `FlashGlyphStore::writesStopped()` covers
  every other reason (the log or the index filled, a write failed, a store opened with the wipe pending), and those
  still stop all writes.
- **Taking the last free entry makes the wipe pending too,** so the next book open empties the table before a new
  body size needs an entry.

### 4.7 Wear

Sectors are erased only at install (family change), on a wipe, or on a reinstall after an update,
**and then only sectors that hold data.** An erase is what wears the chip, and on the X3 it costs
4-6 ms even on a blank sector (Appendix A). So a wipe after the size table filled erases a few sectors,
not 1.3 MB, and an install over a wiped store erases almost nothing. Everything else programs erased
bytes once; batching those writes (4.5) saves time, not erases. Typical SPI NOR flash allows about 100,000 erase cycles per
sector, so even one family change a day lasts centuries. The partition is separate from the app
partitions, so firmware updates do not wear it. Write time is the real cost: while flash is being
programmed, the CPU cannot fetch code, so it stalls. That is about 0.7 ms per 256-byte page programmed
and about 45 ms per 4 KB sector erased, from typical datasheets; step 0 measures the real chip.

With native sizes (2c-2) a book claims its handful of sizes, so the 32 entries fill every few books and wipes come
more often. Each still erases only the sectors that hold data.

## 5. Font source and engine

### 5.1 `FontBytes`

**As built (Stage 2b, `lib/EpdFont/ttf/FontBytes.h`):** the flash mode, a pointer and a length, with the
reads inline and both flags. Stage 2c adds the SD mode behind the same calls.

`FontBytes` is the engine's only way to read a font. It offers `u8/u16/u32(offset)` and
`read(offset, len, dst)`, and every access is bounds-checked. Two error flags are kept apart, because
6.1 treats them differently:

- **out of range:** the font itself is broken, which is permanent;
- **I/O error:** an SD read failed, which is transient.

Modes:

- **Flash mode:** a pointer and a length into the style's mapping.
- **SD mode:** offsets inside a copied table are read from the mapping. Everything else (the glyph
  outlines) comes from an open `HalFile` through an LRU page cache of 8 × 512 B. That 4 KB of heap is
  held while the font is loaded, in SD mode only.
- **Outline reads in SD mode must move forward through the file.** A backward seek makes SdFat walk the
  FAT chain from the start of the file. On a 19.7 MB font that costs 4.5 ms per random 512 B read, against
  0.35 ms sorted. Prefetch therefore renders misses in glyph-ID order (3.2).

### 5.2 `TtfRasterizer`

`render(FontBytes&, glyphId, pxPerEm26_6, Scratch&) -> 2-bit bitmap + metrics`.

- **Integer TrueType loader:**
  - cmap formats 4 and 12; loca, both short and long; simple glyphs;
  - compound glyphs with xy offsets and 2.14 scale or matrix transforms (point matching deferred);
  - hmtx advances, and hhea ascender/descender.
  - Font units are scaled to 26.6 with integer multiply and divide. No floating point is allowed
    anywhere.
  - A build check enforces this: `scripts/check_no_float.py` runs after every link and fails it when an
    object under `EpdFont/ttf/` has an undefined soft-float or libm symbol (`nm -u`). It reads the
    objects, not `firmware.map`: the map credits each libgcc member only to the first object that pulled
    it in, and SdFat and the battery monitor already pull in the double routines.
  - **As built (Stage 2b):** `ttf::Rasterizer` owns the scratch block (5.3). `load()` reads and scales an
    outline and reports the `EpdGlyph` metrics; `rasterize(dst)` draws it anywhere, so 2c can render
    straight into `GlyphBatch::add()`'s pointer; `render()` does both into the output region. Scaling is
    FreeType's (`FT_MulFix` by `FT_DivFix`'s scale, transform before offset), and each outline is moved
    by the phantom point `xMin - lsb` of the top glyph or of a `USE_MY_METRICS` component. Advances are
    computed in 64 bits; a glyph whose advance, size or position does not fit `EpdGlyph` is Unsupported.
- **`ftgrays.c`** (FreeType 2.13.3, the version the SDK vendors) is built with `STANDALONE_`:
  - It is vendored under `lib/EpdFont/ttf/third_party/freetype/`, with `FTL.TXT` and a PATCHES note.
  - Shims: `FALL_THROUGH`, `FT_BEGIN_HEADER`, `FT_STATIC_BYTE_CAST`.
  - `ft_grays_raster` is renamed so it cannot clash with the SDK's FreeType.
  - Patched to take its cell pool from the scratch block instead of the stack. Unpatched,
    `gray_convert_glyph` has a 4,400 B frame.
  - **Decided 2026-10-14 (user): it defines FreeType's 64-bit types (`FT_INT64`).** A standalone build
    otherwise takes an older line renderer and conic flattening than every full FreeType build. That costs
    one-pixel differences from FreeType and makes the output depend on the width of `long`, so host and
    device disagree. The 64-bit path is about 16% slower per glyph (Appendix A, "Stage 2b engine"), about
    2 ms on a page's first contact. It is kept for output identical to FreeType and an exact host/device
    check.
  - It renders through the direct span callback (`FT_RASTER_FLAG_DIRECT`). Spans are packed straight
    into the 2-bit output with the PC converter's thresholds (coverage nibble ≥12 → 3, ≥8 → 2, ≥4 → 1),
    so there is no 8-bit coverage buffer.
- **Sizes match the PC converter:**
  - pixels per em = pt × 150 / 72;
  - advances in 12.4 fixed point from the unscaled `hmtx` value;
  - ascender, descender and line height from FreeType's face metrics (`hhea`, or OS/2's typo metrics when
    the font asks), rounded like `fontconvert_sdcard.py`.

  So a TTF at 14 pt lays out like a `.cpfont` at 14 pt. Only the layout matches: the converter
  auto-hints its bitmaps, so a TTF's glyph boxes and pixels match FreeType unhinted, not the `.cpfont`.

### 5.3 Scratch memory (resident while a TTF family is loaded)

One block, allocated at family load and freed at unload. Its size is computed from the font, with no
allocation per glyph:

- **cell pool:** 4 KB, or 8 KB for an SD-mode (CJK) family. The pool size never changes the output
  (host check: identical bytes), only the speed; see the scratch row in section 2;
- **outline points:** 9 B per point, sized from `maxp` (the larger of maxPoints and
  maxCompositePoints), capped at 512 points (4.6 KB);
- **contours:** 2 B each, from `maxp`, capped at 256;
- **2-bit output:** sized from the head bbox at the largest rung, capped at 4 KB. As built, never smaller
  than the em square (FreeType does not trust head's box either), and the plan records that largest size
  (`ScratchPlan::maxPpem26_6`): a glyph too big for the region at a larger size is Transient, the plan's
  fault, while one too big at a planned size is Unsupported.

That comes to about 6-10 KB for text fonts and about 14 KB for a CJK family. The block is not borrowed from the lent framebuffer,
because section builds already use that region for their build arena. A glyph with more points than
the cap can never load (6.1); text glyphs stay well under it.

### 5.4 Kerning and ligatures

- **Code:**
  - `Gsub.{h,cpp}` is copied from FreeInkFont at a pinned SDK commit, with attribution.
  - `Gpos.cpp` is NOT used for lookups: it re-parses the script, feature and lookup lists on every call
    and scans format-2 ranges linearly (137 µs/pair on the X3).
  - Our GPOS reader resolves, once per style at load, the ordered list of `kern` PairPos subtables
    (required feature first, then the language system's features, lookups and subtables; Extension
    lookups unwrapped). Per pair it applies them first-found-wins, with binary search in format-2
    coverage and class tables, which OpenType keeps sorted.
  - Host check: identical to `Gpos.cpp` on every repertoire pair of six fonts. On the X3: 37.7 µs/pair
    for Bookerly, 23 µs for Noto Sans.
- **Kerning:** `TtfFont::kerning(l, r)`:
  1. probes the mapped pair index (4.3);
  2. on a hit, scales the stored font units to 4.4 pixels at the face's size, clamped to int8 like the
     `.cpfont` matrix;
  3. on a miss, asks the GPOS reader and writes the pair's slot (font units, a zero included).

  Measured on the X3: 0.45 µs/pair warm. About 230 slots are written on a first page and about 400 more
  over a first chapter, each slot write costing 124 µs.
  - There is no RAM pair cache: the warm store is already faster than one (2.0 µs).
  - Layout and draw use the same values, as built-in fonts do. SD fonts today kern only when drawing.
- **Ligatures:**
  - fi, fl, ff, ffi and ffl are resolved once at load through GSUB and become an `EpdLigaturePair`
    table per style. It is shared by all of that style's faces and is a few dozen bytes.
  - A ligature glyph with no codepoint of its own gets the private-use key `U+F0000 + glyphId`, which
    `TtfFont` maps back to the glyph ID.

**As built in 2c-3 (2026-10-16).** Where it differs from the above:

- **One set of readers, the SDK's.** FreeInkOpenType (freeink-sdk PR #7, merged as `e140f74`) builds `Gpos`,
  `Gsub` and a new `KernTable` from FreeInkFont's sources, without FreeType. FreeInkFont's `FtFont` uses the same
  code. The app keeps only glue (`lib/EpdFont/ttf/TtfOpenType`). Nothing is copied.
  - **GPOS:** the union of every script's DefaultLangSys `kern` lookups (required feature included), not
    `latn`/`DFLT` alone. The lookups apply in LookupList order: the first matching subtable counts within a
    lookup, and the lookups add up. That is a shaper's result when a font points every script at the same lookups,
    as most do. A font that splits its kern lookups by script gets their union.
  - **Resolved once:** a style resolves its PairPos subtables into byte offsets, at most 256 of them (the most
    found in about 30 real fonts is 76), and asks the resolved overload per pair. A per-call walk stops at 4,096
    steps, a resolve at 65,536, and out-of-range feature indices count against the budget.
  - **The legacy `kern` table** (version 0, horizontal format 0) kerns a font whose GPOS has no kern subtables. It
    matches `FT_Get_Kerning` on sorted subtables, but it skips cross-stream subtables, which FreeType adds in.
  - **GSUB** keeps FreeInkFont's policy (`latn`, else `DFLT`), now on the shared view.
- **Ligatures are keyed by U+FB00-FB04,** not `U+F0000 + glyphId`: `EpdFont::getLigature` takes BMP pairs, and
  ffi/ffl chain through ff, as `EpdFont` applies ligatures two at a time. `glyphFor()` maps those codepoints to the
  glyphs GSUB names, whatever the cmap says. A ligature glyph outside the font (`>= numGlyphs`) is dropped, so the
  text draws as separate letters.
- **Kerning:**
  - **Scaling:** a stored pair is scaled to 4.4 pixels at the size's own ppem, rounded half away from zero and
    clamped to `int8`. That caps kerning at ±7.94 px: at a 120 px native size, -120 units (-14.4 px) clamps to -8.
  - **Pairs that kern nothing** are not stored. A 74 B bit set per style remembers left codepoints below U+0250
    that kern with nothing (space and digits in Bookerly), so each is asked once per family load. It is not a
    pair cache.
- **Versions:**
  - `kRenderVersion` 2: a store from 2c-2 could hold the cmap's glyph, or a no-glyph marker, for U+FB00-FB04, so
    every family reinstalls once.
  - `SECTION_FILE_VERSION` 83.

### 5.5 Sizes: the body face and the ladder rungs

- **The body face** is the reader size the user has chosen, at any point size the settings allow,
  rendered natively. A TTF family never uses the scaled alias (`ensureSizeAlias`).
- **Rungs.** `buildReaderFontSizeLadder` gets a TTF branch. For a TTF body font it adds one rung per
  `FONT_SIZE_RUNGS` entry:
  - font ID `computeFontId(identity, family, rungPt)`;
  - `sizePct = rungPt * 100 / bodyPt`.

  This is the same shape as the built-in ladders, so `FontSizeLadder::resolve` and its 3% dead zone
  are unchanged. A rung equal to the body size is the body face.
- **Dormant until needed.** At family load, `SdCardFontManager` keeps a dormant table (font ID → rung
  point size). Nothing is allocated and nothing is entered in `fontMap`. The lookup helper's resolver
  (3.3) creates the face on first use. That covers every lookup inside `GfxRenderer`: layout, scan
  passes and draws, including pages laid out in an earlier session. It does NOT cover code that reads
  `fontMap` directly: `FontCacheManager::prewarmCache` (its own `fontMap_` reference), FreeInkUI
  (`getFontMap().find`) and `SdCardFontManager`. A cached page drawn in a fresh session therefore
  reaches `prewarmCache` while its rung is still dormant, so the TTF branch of `prewarmCache` must
  resolve through `TtfFont`'s dormant table, not `fontMap_`; otherwise that page loses the batched
  prefetch and renders its glyphs one at a time (correct output, but about 4.76 against 0.73 ms per CJK
  glyph in SD mode).
- **Lifetime.** Faces live until the family is unloaded. There is no eviction in v1: at most 9 rungs,
  about 0.5 KB each.
- **Store use.** Each face has its own size index. Rung sizes are absolute, so a body-size change adds
  at most one new entry (the new body size, if it is not a rung). The 16 entries cover all rungs plus
  several body sizes.

**As built in 2c-2 (2026-10-15)** (plan `plans/2026-10-15-ttf-stage2c2-native-sizes.md`):
- **Whole-pixel sizes instead of rungs.** A size is drawn at `round(bodyPx * pct / 100)` px, so the leftover scale
  stays inside the ladder's 3% dead zone and nothing is resampled. The nearest 2 pt rung would leave several percent
  to resample.
- **No dormant table:** a size's font ID is `base | px`, in a window of 256 IDs derived from the family identity.
  `FontSizeLadder::setNative()` makes the IDs; the resolver decodes them and makes the face on the first lookup.
- **10 px up to the family's ceiling.** The ceiling is 120 px, less where the tallest glyph (`head.yMax`) or the
  furthest left (`head.xMin`) would leave `EpdGlyphRef`'s `int8` (Bookerly 119 px). Above it, the largest face is
  drawn scaled.
- **At most 16 sizes, dropped at book open, at a body size change and at unload.** When all 16 are held, or memory
  is short, the nearest size held stands in. It is registered scaled to the requested size (`ResolvedFont`, a base
  scale, as for `insertScaledFont`), so a page laid out with the ID in another session keeps its measurements.
- **No auxiliary-face budget** in `ChapterHtmlSlimParser` for a native ladder, and a drop cap resolves its own size.
  `SECTION_FILE_VERSION` 82.
- **The prewarm** looks fonts up through the renderer, so it reaches sizes not looked up yet; there is no dormant
  table to consult.
- **Inline sizes stay scaled:** a word whose own size differs from its block's is the block's face scaled.

### 5.6 Not supported in the first version

- compound glyphs positioned by point matching: the glyph is logged and drawn as missing;
- glyphs with more than 512 points;
- font collections (`.ttc`);
- variable fonts beyond their default instance: they register as Regular, and missing styles fall back
  as an incomplete `.cpfont` family does;
- Apple's scaled component offsets (`SCALED_COMPONENT_OFFSET` with a transform and a non-zero offset):
  Unsupported, like point matching;
- embedded bitmap strikes: glyphs always come from the outlines;
- FreeType's "tricky" fonts, whose glyphs are assembled by bytecode (for example DFKai-SB): they would
  render garbled, so the registry should refuse them (8.1);
- hinting.

See section 11 for the full deferred list.

## 6. Errors and limits

### 6.1 Glyph failures

| Situation | Behaviour |
|---|---|
| The font has no glyph for the codepoint | A slot with the "missing" marker; the replacement glyph is drawn as today. |
| A permanent load failure (`Unsupported` or `Malformed`: point matching, over a cap, an out-of-range read in the outline, an advance or position `EpdGlyph` cannot hold) | A slot with the "missing" marker; logged once. |
| A transient failure (`Transient`: an SD I/O error, no scratch block, or a size above the one the scratch block was planned for) | Drawn as missing this time, **not** marked, retried at the next lookup. |

### 6.2 Store and mapping failures

| Situation | Behaviour |
|---|---|
| A flash write fails | Set wipe pending, switch to ring mode, log it. |
| The index or log cannot be mapped | Reinstall with smaller regions. If that also fails, use ring mode for the session; log it. |
| The TTF file was changed or removed | The identity check at load reinstalls, or the family drops out of the registry and the reader falls back as it does today. |

### 6.3 Budgets

- **Resident heap per loaded TTF family:**
  - scratch, 6-10 KB (about 14 KB for a CJK family, with the 8 KB pool);
  - the GPOS reader's subtable list, about 100-200 B per style;
  - the SD page cache, 4 KB, in SD mode only;
  - the ligature tables, a few dozen bytes;
  - about 0.5 KB per face in use (540 B measured on the X3): the body face plus a book's native sizes, at most 16
    (2c-2);
  - the ring's bitmaps, at most 8 KB (2c-2).

  Every font table and every stored bitmap is mapped, not copied, and there is no per-page arena. That
  comes to about 7-14 KB for a Latin family (about 19 KB for CJK in SD mode), against roughly 9-14 KB for an SD `.cpfont` (about 6 KB of kern classes plus
  a 3-8 KB arena per page).
- **Ring mode only:** the 8-slot heap ring, allocated when entering ring mode and freed at the wipe or
  at unload.
- **Flash for the code:** about 20-25 KB. The engine is 6.4 KB and `Gpos`/`Gsub` about 4 KB; the rest is
  `TtfFont`, the store, `FontBytes`, the provider and lookup changes, and the registry. Measured in the
  build and quoted in the PR.

## 7. Data flow

### 7.1 Discovery and install

1. `SdCardFontRegistry` also scans `.ttf` files. Each file's family comes from the `name` table (ID 16,
   or ID 1 if there is no 16), and its style from `head.macStyle` (bold and italic bits). A registry
   entry records its kind, TTF or `.cpfont`.
2. For a TTF family, `SdCardFontManager::loadFamily` creates a `TtfFont` instead of an `SdCardFont`.
   `TtfFont` computes the family identity.
3. If the store header matches, the store is reused. Otherwise the family is installed behind a
   "Preparing font…" popup:
   - flash mode: erase, then copy every style. About 1.8 MB, roughly 10 s;
   - SD mode: erase, then copy the tables. About 3-5 s for CJK.
4. The index, the log and (in SD mode) the tables are mapped, the log is scanned to find where writing
   resumes, and the scratch block is allocated.
5. The body face is registered in `fontMap`, the rungs are recorded as dormant, and the resolver is
   installed.

**As built in 2c-1 (2026-10-15):**
- **A family is a directory** of `.ttf` files under a fonts root, as `.cpfont` families are; the `name` table is
  not read. A directory with any `.cpfont` file stays a `.cpfont` family.
- **Style** from `head.macStyle`; each style takes, of its files, a static one before a variable one, then the
  `OS/2.usWeightClass` nearest 400 (regular, italic) or 700 (bold styles), then the first by name. A regular
  style is required.
- **Too large for flash mode:** such a family is not listed at all until SD mode (2c-4); the check is
  `glyphstore::planLayout` against the font area and the mapping budget at discovery.
- **Sizes:** a TTF family offers 8 pt and the 10-26 pt ladder (`ReaderSizeList::native()`), drawn natively; a
  size change registers a new font ID without a reload.
- **The wipe** runs at family load and at book open (the explicit-family `ensureLoaded`), behind the "Loading
  font" popup; not before a section build.
- **A style that no longer maps** after a firmware update (the budget shrank) reinstalls the family once.

### 7.2 Layout and drawing

Layout and drawing both go through `EpdFont`. Before a text run is measured (`ensureFontReady`) and
before a page is drawn (`prewarmCache`), `TtfFont::prefetch` renders that text's missing glyphs in
glyph-ID order (3.2). A glyph lookup:

1. probes the store and returns the record pointer on a hit;
2. on a miss, renders the glyph, writes it through to the log and the index (4.5), and returns the
   stored record.

- **Most rendering happens during section builds,** so page turns mostly hit the store.
- **Kerning comes from the provider** (the pair index, then the GPOS reader), both in layout and when
  drawing (5.4).

### 7.3 Ring mode

Ring mode applies when wipe is pending, after a write error, or for previews. It is the same flow,
except that a glyph that is not stored is rendered into the 8-slot heap ring and is not written. Glyphs
already in the store are still read from it.

As built in 2c-2: a size the full table refused is drawn through the ring while the family's other sizes keep
storing. The ring keeps within 8 KB by freeing its oldest slots first (a record is promised only until the next
miss), and it is freed with the native sizes.

### 7.4 Size and family changes

- A new body size gets a new body face and, unless it is a rung, the next size-table entry. Its glyphs
  build up alongside the others.
- Rung faces do not change, because rungs are absolute point sizes.
- A new family means a reinstall.
- The family identity feeds `computeFontId`, so changing the TTF files gives new font IDs and the
  sections are laid out again.

### 7.5 Wipe

The wipe runs when wipe is pending, at book open or before a section build starts, when no record
pointer into the log can be live (4.6).

### 7.6 Previews (font selector)

A TTF family that is not installed is previewed through a temporary `TtfFont` in ring mode, reading
from the SD card. **Nothing is written to the store,** so a preview never wipes the installed family;
`.cpfont` previews follow the same rule with `FlashCachePolicy::ReadOnly`. A preview sentence has about
18 distinct glyphs, more than the ring's 8 slots, so some are rendered again across the measure, wrap
and draw passes. At about 1 ms per glyph that is acceptable; step 0 confirms it. The rendered previews
are cached as `.fpv` files, keyed by the family identity.

## 8. Delivery stages

| Stage | Content | Merges? | Status (2026-10-13) |
|---|---|---|---|
| **Step 0** | Throwaway device benchmark on the X3 (section 9.1). | No | Done; Appendix A |
| **Stage 1** | Two changes, neither altering behaviour: (a) `GlyphProvider` in place of `glyphMissHandler` and its context; `SdCardFont` implements it and `getGlyphBitmap` loses the cast. (b) `GfxRenderer`'s single font-lookup helper plus the optional resolver hook. | One PR (the plan's Task 5) | PR #468 |
| **Stage 2a** | `FlashGlyphStore` and its host tests (simulated NOR flash, power-cut injection). | Own PR | Merged (PR #469) |
| **Stage 2b** | `TtfRasterizer`: integer loader, vendored and patched `ftgrays`, span packing to 2-bit, and the regression test against FreeType. | Own PR | PR #470 (built and reviewed 2026-10-15; plan `plans/2026-10-14-ttf-stage2b-rasterizer.md`) |
| **Stage 2c** | `FontBytes`' SD mode, `TtfFont` (with `prefetch` and its two hooks), the pre-resolved GPOS reader and the kerning pair index, `Gsub`, registry and manager wiring, dormant rungs and the TTF ladder branch, previews. Split into four plans: **2c-1** TTF families at the body size, flash mode; **2c-2** native sizes (headings, CSS sizes, drop caps at their own pixel size); **2c-3** kerning and ligatures; **2c-4** SD mode, and previews (a family that is not installed is read from the SD card). | Own PR each | 2c-1 merged (PR #472); 2c-2 merged (PR #474); 2c-3 built, reviewed and X3-validated 2026-10-16 (SDK PR #7 merged; app branch `feat/ttf-kerning`; plan `plans/2026-10-15-ttf-stage2c3-kerning-ligatures.md`); 2c-4 next |
| **Stage 2d** | User-guide section on TTF fonts; FreeType credit (required by the FTL licence). | With 2c or own PR | Not planned |

Each stage builds on the previous one. Stacked PRs are retargeted to master as their base merges.

**Planning.** Each stage gets its own implementation plan, written when the previous stage's results
are in. Step 0 and Stage 1 come first and have no dependency on each other. Stage 2's plans use step
0's numbers.

**Branches and files** (all pushed to origin, 2026-10-14):

| Branch | What it holds | Fate |
| --- | --- | --- |
| `docs/ttf-fonts-design` | This spec, the step 0 plan and the Stage 1 plan. | Working material: merges with or before the code, then is folded away (docs layout convention). |
| `bench/ttf-step0` | The step 0 bench, `bench/ttf/` and `bench/ttf_main.cpp`, with a README on regenerating its data, running it and rebuilding its host checks. It also holds the prototype: the integer loader (`ittf.c`), the standalone `ftgrays` builds, the pre-resolved GPOS reader (`fastkern.h`) and the kerning options (`kernopts.h`). | Never merges. Stage 2b and 2c start from its prototype code; delete the branch once Stage 2 no longer needs to re-measure. |
| `refactor/glyph-provider` | Stage 1. | PR #468 to master. |

The CJK font used for measuring (Microsoft YaHei) is a Windows system font: use it locally only, and
never commit it or data generated from it.

### 8.1 Carried into Stage 2 from the Stage 1 review

The Stage 1 final review (2026-10-13) found nothing to fix in Stage 1 itself. These points bind the
Stage 2 plans:

- **Resolver writes and concurrency (2c).** Once a resolver is installed, any lookup can insert into
  `fontMap`. On the S3 the loop and render tasks run in parallel, so an unlocked reader would race an
  insert that rebalances the map. Either assert `RenderLock` in the resolve branch (as 3.4 does for the
  miss path) or create rung faces at a safe point.
- **The resolve path allocates in the middle of a draw** (a map node of about 40 B plus a face of about
  0.5 KB), and `std::map::emplace` aborts on OOM because the firmware has no exceptions. Prefer creating
  rung faces during section builds or at family load; if they are created lazily, create them with
  `nothrow` allocation before the emplace.
- **`prewarmCache` and the dormant table:** see 5.5.
- **`TtfFont::ownsBitmap` should be O(1)**, an address-range check against its ring, because every draw
  from the store pays that call.
- **Record lifetime.** `GlyphProvider::missGlyph` promises a record only until the next miss on ANY
  provider of the same font (SdCardFont's styles share one ring; `TtfFont`'s per-face adapters over one
  ring will too). `EpdGlyphRef` copies the metrics by value, so layout may hold any number of refs; only
  `sdRecord` (bitmap offset and ownership identity) ages, and draws consume it straight after the lookup.
  Rename `sdRecord` in 2c, since it will carry flash-store records too.
- **From the Stage 2a audit (2026-10-14), for 2c:**
  - **Kerning pairs:** look up whether the left glyph is in any pre-resolved PairPos coverage before
    asking GPOS or storing a pair (about 1 µs). Storing every pair, zeros included, fills the CJK pair
    index in the first chapter (about 0.7 s of slot writes) and is the larger share of a first Latin
    page's flash writes.
  - **Batches:** use the scratch block's output region as the `GlyphBatch` buffer and render straight
    into `add()`'s pointer (it starts as 0xFF, solid ink, so every byte must be written); commit when
    `add()` returns null and at the end of each prefetch; prefetch per paragraph or page rather than per
    run. On PSRAM boards keep batch buffers in internal RAM (`EspNorFlash` bounces external sources
    through 512 B, at one write call per 512 B).
  - **Styles:** group a page's misses by style; in flash mode each `mapStyle` switch remaps about 8
    pages. Consider mapping one style's tables at a time in SD mode too: a 2-style CJK family gets a
    14-page log instead of 21.
  - **Size indices:** re-claim a face's index when `generation()` changed; a wipe voids every index.
  - **Lifetimes:** `find()` pointers, `logBase()` and `mapStyle()` pointers die at `close()`, `open()`,
    `beginInstall()` and `wipe()`; in flash mode `mapStyle()` for another style unmaps the previous one.
  - **Planner input:** derive the expected record size from the body pixels-per-em at install (records
    grow with its square); for CJK at 12-14 pt that is 10-20 % more glyphs before a wipe.
  - **Partition ownership:** close the TTF store before loading a `.cpfont` family (4.1).
- **From the Stage 2b review (2026-10-15), for 2c:**
  - **Field widths:** `EpdGlyphRef` narrows a provider glyph's left and top to `int8_t`, and
    `EpdFontData::advanceY` is `uint8_t`. Cap the rung sizes so they fit, or widen `EpdGlyphRef`; refuse a
    face whose `lineMetrics().advanceY` is over 255.
  - **Batch records:** a `GlyphBatch` record takes `align4(20 + dataLength)` bytes, but the output region
    is sized for the bitmap alone; as the batch buffer, it must leave room for the record header.
  - **`add()` then `rasterize()`:** a staged record cannot be taken back, and `rasterize()` can still fail
    (an ftgrays overflow). Decide what fills that slot. `render()` writes `output()`, so it must not be used
    while the output region is the batch buffer.
  - **SD errors:** `glyphIndex()` returns 0 on an SD read error too; check `ioError()` before marking a
    codepoint missing. Bound format-4 cmap reads by `cmapEnd` before the SD mode, or a broken cmap reads
    stray SD sectors.
  - **Default-ignorable codepoints:** advance 0 and no ink, as the converter does.
  - **Time:** a hostile glyph (512 points of full-width zig-zag conics) can take about 0.5-0.7 s on the X3
    (estimate). Prefetch yields between glyphs.
  - **One render at a time:** `ftgrays`' standalone raster object is a static. Assert `RenderLock` around
    every `Rasterizer` call (debug builds); overlapping renders could overflow a pool.
  - **Tricky fonts:** the registry refuses FreeType's bytecode-assembled "tricky" fonts (5.6).
  - **S3:** a scratch block over 4 KB lands in PSRAM unless allocated internal.
- **Optional:** a `prefetch(text)` virtual on `GlyphProvider` (no-op by default) would let
  `ensureFontReady` and `prewarmCache` reach `TtfFont` through `family->getData(style)->provider`,
  without a font-ID-to-TtfFont registry.
- **Done in 2c-1 (2026-10-15):** `GlyphProvider::prefetch` (through `EpdFontFamily::prefetch`); field widths
  (sizes up to 26 pt, `advanceY` over 255 refused, glyphs whose left or top do not fit `int8` marked missing);
  batch records (the output region grows by the record header); add-then-rasterize (`GlyphBatch::dropLast()`);
  `ioError()` checked before marking a codepoint missing; default-ignorable codepoints (no advance, no ink);
  yields between glyphs and install chunks (a tick every 200 ms); the RenderLock check (logged once, not
  asserted); O(1) `ownsBitmap`; the ring's record lifetime; size indices re-claimed after a wipe; partition
  ownership (the `.cpfont` mapping released before the store opens, the store closed at unload); one style mapped
  at a time.
- **Done in 2c-2 (2026-10-15):**
  - **The resolver:** a size is made on its first lookup, under `RenderLock` (checked when a size is made), with
    `makeUniqueNoThrow`. When no size can be made, a scaled stand-in serves. Only the `fontMap` node (about 48 B)
    is not nothrow.
  - **The prewarm** goes through the renderer's lookup, not a dormant table.
  - **The drop cap:** native up to the family's ceiling, scaled above it (5.5).
- **Done in 2c-3 (2026-10-16):** kerning pairs in the store's pair index (font units, codepoint keys) and the
  coverage pre-check: a pair whose left glyph kerns with nothing is never stored (5.4).
- **Still open, by plan:** `cmapEnd`-bounded format-4 reads, the SD-mode record-size planner input, grouping a page's misses by style, tricky fonts and the S3's internal-RAM scratch
  (2c-4); the `sdRecord` rename.
- **For 2c-2, from the 2c-1 device run (done in 2c-2):** a drop cap is drawn at about 4x the body size (a 16 pt body gives
  about 133 px per em). That is above the 26 pt the scratch block is planned for, near the 4 KB output region, and
  at 6x it passes `EpdGlyphRef`'s `int8` top. Native sizes need a larger planned maximum, and a resampled fallback
  above what the region and `EpdGlyphRef` hold.

## 9. Testing

### 9.1 Step 0: device benchmark (throwaway, X3), DONE 2026-10-13

Its numbers are in Appendix A; the decisions they drove are in section 2. Gate verdicts:
- **Latin all-in:** about 1.3 ms per new glyph, with the cache emptied and the writes measured in place:
  PASS.
- **CJK all-in:** 2.5-2.9 ms per new glyph with file-order reads: over 2 ms, so REVISIT. The first-chapter
  gate passes (3.8-4.3 s for 1,500 glyphs) only with file-order reads (about 10.4 s in text order). The
  remedy taken is batching misses in glyph-ID order (3.2, 5.1). Metrics-only layout stays deferred.
- **Free mapping pages:** 32: PASS.

What was measured:

- µs per glyph for a Latin font from mapped flash, and a CJK font with its tables mapped and outlines
  read from SD;
- a store hit as a hash probe through mapped flash, against today's mini-table lookup;
- a GPOS kerning lookup, with and without the pair cache;
- the stack high-water mark of the loop task and the render task while rendering;
- flash program time per stored record, and the erase time for 64 KB and 1 MB;
- free mapping pages at runtime.

Gates:

- **If a glyph costs more than about 2 ms all-in** (render plus write), or **a CJK first chapter would
  add more than about 10 s to its build,** pull the deferred metrics-only layout mode (section 11) into
  Stage 2c.
- **If fewer than about 25 mapping pages are free,** shrink the store regions in 4.1.

### 9.2 Stage 1

Stage 1 must not change behaviour:

- All existing host tests pass, and page-dump goldens are byte-identical.
- A device smoke test with a `.cpfont` family and with a built-in family: page turns, chapter builds,
  previews, the dictionary view, and ring glyphs (force a prewarm miss). The heap low-water mark
  (`logStats`) does not get worse.

### 9.3 Stage 2: host tests

- **`FlashGlyphStore` on a simulated NOR flash.** Programming can only clear bits, erase sets 0xFF, and
  the test fails on any write that needs 0 → 1. A power cut is injected at every write step, then the
  store is reopened. The index must be consistent, no torn record may be referenced, and the write
  position must be correct. Also covered: the full and wipe-pending transitions, and the size-table
  limit.
- **The integer loader against FreeType unhinted** (the host build has FreeType through FreeInkFont),
  over every glyph of the bundled TTF sources: no load failures, ≤5 box differences per font, and ≤3% of
  pixels different at 2-bit. These are today's prototype numbers kept as a regression gate. Span
  packing must match packing the bitmap afterwards.
  **As built (Stage 2b, `test/ttf_rasterizer/`), much stricter:**
  - every glyph index of all 12 bundled TTFs at 8, 14 and 40 pt: no load failures, no advance or box
    differences, and every bitmap byte-identical to FreeType's (the reference clears the OVERLAP flag);
  - the cmap identical up to U+2FFFF, and the line metrics equal to the converter's at six sizes;
  - synthetic compounds (rotated, scaled, mirrored, `USE_MY_METRICS`, nested) byte-identical too;
  - against FreeType's oversampled rendering of overlap-flagged glyphs: 0.77-0.85% of pixels at 14 pt
    (≤3%);
  - damaged fonts: damage aimed at what a render reads (the glyph's record, its `loca` and `hmtx` entries,
    `maxp`, `head`), the scratch block planned from the damaged face, and an assertion that the damage
    reaches the loader; ASan and UBSan on CI's Linux GCC, UBSan in trap mode with MinGW;
  - a host/device hash: the X3 bench (`env:bench_ttf_raster`) draws exactly what the host checked.
- **`TtfFont` cold against warm:** the same layout metrics and the same page dumps with an empty store
  and with a populated one. Ring mode must give the same dumps as stored mode.
- **`FontBytes`:** bounds checks, SD page-cache eviction, and routing between copied tables and SD.
- **Corrupted fonts** under AddressSanitizer and UBSan: truncated files and randomly mutated tables. No
  crash and no read out of bounds.

### 9.4 Stage 2: device (X3 and X4)

- a Latin family in flash mode and a CJK font in SD mode;
- headings render from rung faces, including on pages laid out in an earlier session;
- after a reboot nothing is rendered again (the logs show store hits);
- a family switch wipes the store, the store-full path enters ring mode and wipes at the next book
  open;
- first-page latency, first-chapter build time, and the heap low-water mark against the `.cpfont`
  baseline;
- a preview never touches the store.

## 10. Prototype

The prototype is on branch `bench/ttf-step0` under `bench/ttf/` (see its README): `ittf.c` (the integer
loader), the standalone `ftgrays` builds, `fastkern.h` (the pre-resolved GPOS reader) and host checks.
`bench/ttf/host/fullcmp.cpp` is the comparison against FreeType that produced the accuracy figures in
section 2. All of it is throwaway code: Stage 2b reimplements the loader on top of `FontBytes`, keeping
the measured behaviour as its test gate.

## 11. Deferred (pick up after the first version)

Glyph store minors from the Stage 2a audit (2026-10-14), each a speed or tidiness gain, none a defect:

- Erase a whole 64 KB block when at least 3 of its sectors hold data; a wipe's partial-block tail is
  now erased sector by sector (−0.2 to −0.85 s per CJK wipe; measure the sector erase time first).
- Take `usedSlots` from the log scan instead of reading the whole index at open (−11 to −14 ms per
  family load for Latin).
- Read 1 KB at a time in install's blank check (−0.3 to −0.45 s per install).
- An `enum class` for the store mode; tests including `ttf/...` like firmware code; avoid holding the
  648 B header for the whole install.


1. Compound glyphs positioned by point matching (ARGS_ARE_XY_VALUES clear).
2. Glyphs with more than 512 points (scratch cap).
3. Font collections (`.ttc`).
4. Variable fonts beyond the default instance (real bold from the `wght` axis).
5. Hinting: glyphs are unhinted, so stem weight at small sizes can vary (compare issue #149). A light
   integer stem darkening or x-height snap could follow.
6. FreeType's 4×4 oversampling of overlapping outlines. The cause of the 2-3% difference at 2-bit;
   revisit only if artefacts show on the panel.
7. Ligatures beyond fi/fl/ff/ffi/ffl, and other GSUB features.
8. Native rendering at exact CSS sizes beyond the ladder rungs, and evicting faces before the family
   unloads.
9. Several families in the store, for example archiving a family's store to SD on switch.
10. Smarter handling of a full store (generations, LRU) instead of a wipe.
11. Checkpoints for finding the log's write position, if the scan measures slow.
12. A metrics-only layout mode, so a CJK first chapter build need not render every new glyph (step 0
    gate).
13. Apple's scaled component offsets, and embedded bitmap strikes (5.6).
14. FreeType's leniency with broken tables, where the engine fails the glyph or the face instead: a short
    OS/2 table (FreeType bounds it by the file), `loca` entries out of order or past `glyf` (FreeType
    loads, empties or truncates them), a zero-contour glyph's box, cmap subtables FreeType's validator
    would drop, overlapping format-4 segments, and a `maxp` that understates the points.

Rasterizer minors from the Stage 2b review (2026-10-15), none a defect:

- `scripts/check_no_float.py`: also match `long double`, half-precision, complex and `powi` helpers and
  `l`-suffixed libm; fail loudly when an EpdFont build has no TTF objects; name its blind spots (the S3's
  FPU, inline headers used outside `ttf/`); replace only the `gcc` suffix of `$CC`.
- A `kMinPoolBytes` constant for the 2 KB pool minimum; `advance12_4` taking the scale `load()` already
  has.
- CMake: a clear error when the SDK submodule is missing; FreeType's include dir private to
  `FreeTypeReference.cpp`.
- The README's vendored-components list should name `ftgrays` (with the 2d credit); the bench should check
  every `allocate()`; `vendor_ftgrays.py` could read FreeType's version instead of hard-coding it.

Follow-ups the user asked to keep (2026-10-15), both new features that TTF support makes possible. The reader
does neither today: `CssParser` skips `@font-face` and never reads `font-family`, and a section is laid out
with one font ID (per-word data carries only the style bits).

- **A book's embedded fonts, used as that book's font.**
  - **What it would take:**
    - find the fonts in the OPF manifest (`font/ttf`, `application/font-sfnt`, `application/vnd.ms-opentype`,
      ...) and in `@font-face`;
    - undo the font obfuscation many EPUBs use: the IDPF algorithm XORs the first 1,040 bytes with the SHA-1 of
      the unique identifier, and Adobe's the first 1,024 with the UUID;
    - extract the files to a per-book directory on the card and load them as a TTF family through the existing
      per-book font override (`sdFontFamilyOverride`).
  - **Limits:**
    - many embedded fonts are CFF (`.otf`), which the engine refuses (5.6), so those books keep the reader's
      font;
    - embedded fonts are usually subsets, so they install in a second or two;
    - switching between two such books reinstalls the store each time, because it holds one family (4.1).
  - **An option to ignore embedded fonts is needed** either way.
  - **Stage A, as built (2026-10-17, slimmed 2026-10-18, branch `feat/book-fonts`; follows crosspoint-reader PR
    #3934 on our engine):**
    - **The setting:** Reader → Publisher Fonts, on or off (off by default). It applies to a book whose own style is
      in use and that has no font family chosen for it, so the reader menu's per-book Font Family is the per-book
      way out. Off by default because of the store switching below. A toggle while the body font is all it covers
      (stored 0/1, so Stage B's modes can widen it to an enum with 1 still the body). A per-book on/off of its own
      would add a field to the 13-argument override path (`RecentBook`, `QuickOverridesActivity`,
      `applyBookReaderOverrides`): not cheap, so not built.
    - **CSS (`CssParser`, `CSS_CACHE_VERSION` 28):** `font-family` keeps its first family as a case-insensitive
      hash (`fontFamilyHash`); a generic family is 0. It rides in the rule payload, the arena-resident record (bit 3
      of the extra byte, the hash after the multipliers) and the cascade's ranks. `@font-face` rules go to a side
      file, `css_fonts.bin`: name, weight, italic, and the `src` url ranked TrueType, then OpenType, then anything
      else, never WOFF2, SVG, EOT, `data:` or remote.
    - **Which family (`BookFonts::detectBodyFamily`):** the family most of the text is set in, an inline accent's
      text counting for its paragraph's (p, div, li, dd), the style attribute applying, over the first 64 KB of up
      to 6 chapters from the text start, stopping at 16 KB of text. From #3934's `BodyFontProbe` (Julia Nguyen's
      fix), asked once per book instead of per chapter: the book has one font. Measured on 921 real books with
      `@font-face` rules (a local library) against the family each draws most of its text in, every chapter styled
      by the sheets it links: right for 839, a wrong family for 27, a spurious one for 2, missed 53. #3934's further
      rules (html's or body's family first; centred and right-aligned text, headings, tables and hidden text left
      out) scored 807 / 43 / 7 / 64: our parser merges a book's sheets, so a marketing page's `body` rule
      (BICMedia's `marketing.css`, Noto Sans) named a family no chapter uses. A CSS-only rule (the book's one text
      family, or the one html, body or p name) scored 748 / 10 / 19 / 144: not a replacement.
    - **Extraction (`BookFonts::prepare`):** the body family's faces nearest weight 400 and 700, roman and italic,
      go to `<book cache>/fonts/<style>.ttf` through `Epub::extractItemToFile` (the registry reads each face's
      style from the font, not its name), then a state file (`fonts.bin`, `BookFonts::kVersion` 3,
      docs/file-formats.md). Runs once per book, behind the open's "Indexing" popup, with the inflate ring and the
      SAX state in the lent secondary framebuffer. A read failure is retried at the next two opens, then the book
      keeps the reader's font.
    - **Obfuscation:** a face whose first bytes are no TrueType signature is tried with the IDPF key (SHA-1 of the
      unique identifier, white space removed; the first 1,040 bytes) and Adobe's (the urn:uuid's 16 bytes; 1,024),
      undone in place, the same algorithms as `epub-fonts.js`. The key whose XOR gives the signature back is the
      one, so `META-INF/encryption.xml` is not read; real encryption gives none and is refused. SHA-1 is wolfCrypt's
      `wc_ShaHash`, already linked for TLS (0 B of flash for the hash itself). In the 921 books, 56 name Adobe's
      algorithm and 33 IDPF's.
    - **Refused, the book keeps the reader's font:** a generic or non-embedded body family; anything but TrueType
      outlines (CFF `OTTO`, WOFF, WOFF2, collections; the refusal logs the signature); encrypted fonts; a face over
      2 MB; no regular face; a family the font area cannot take in flash mode (the registry's own check). WOFF1 is
      not unpacked: TrueType-flavoured WOFF in EPUBs is rare, and a converter (table directory rewrite plus
      per-table inflate from a temporary file) was estimated at about 1.5 KB of flash.
    - **Loading:** the directory is scanned by `SdCardFontRegistry::scanFamily`, the code that lists a `.ttf`
      directory on the card, and becomes the book family `book:<name>` in `SdCardFontSystem` (a `:` no directory
      name has). It loads through `SdCardFontManager::loadFamily` like any TTF family: native sizes, kerning,
      ligatures, flash mode. Its font IDs come from the family identity (file names, sizes, times), so a changed
      setting or new files lay sections out again.
    - **First open:** `goToReader` leaves the loaded font alone while the book is unexamined
      (`ensureSdFontLoadedForPath(path, waitForBookFonts)`), so the global family is not installed only to be
      replaced a second later; `ReaderActivity` examines the book, then loads.
    - **Store switching:** the store holds one family (4.1). Opening a book in its own font installs that family
      (an erase of the sectors that hold data, then the copy: about 5 s per MB on the X3, 1-2 s for a usual subset);
      the next book in another family, or a book in the reader's TTF family, installs again and renders its glyphs
      again (about 1.3 ms each on first contact). A built-in reader font costs nothing to switch back to.
    - **X4 test (2026-10-18, serial test API):** `/test/bookfont_ttf.epub`, `_idpf` and `_adobe` open in
      `book:Book Body` (Ubuntu letterforms in the screenshots), each examined once behind the popup in about 2 s;
      `_cff` is refused (`not TrueType (4f54544f)`) and stays in Bookerly; a second open trusts `fonts.bin` and
      examines nothing; a book without `@font-face` afterwards is in Bookerly again, its heap back (66.6 KB free,
      65.2 KB before the font books). With a book family loaded, the first look-ahead build fell back from
      Background-C for low heap (30 KB free, 21 KB contiguous) and rebuilt incrementally.
    - **Cost, `env:default` (C3), against master 3afcd0d11 (6,321,493 B):** flash +10.4 KB (6,331,933 B), static
      RAM +72 B. Rebased onto that master it was +12.5 KB; the slimming of 2026-10-18 took 2.1 KB: the setting as a
      toggle (-0.3 KB), the probe's simpler rule (-0.4 KB), the faces by style number and "TrueType or not" instead
      of a classification (-1.1 KB), smaller pieces (-0.3 KB). By symbol: the CSS side 2.8 KB, `BookFonts`' copy,
      keys and state 2.5 KB, the probe 1.9 KB, the reader wiring 1.4 KB, `std::string` helpers GCC clones into the
      new files 0.6 KB, `ContentOpfParser`'s identifiers 0.4 KB, the setting and its name in 24 languages 0.3 KB.
      **MMU pages: 97 (42 code + 55 rodata) against master's 96 (41 + 55):** master's code ended 4.2 KB short of
      its 41st page and the feature adds 9.6 KB of code, so the map window for the font area has one page fewer.
      Getting the page back would take another 5.5 KB of code. No resident heap beyond the TTF family's own;
      `prepare` holds the face records (the side file, usually well under 1 KB), the probe's 0.7 KB, the package
      parser's 1 KB while it reads the keys, and the CSS index while it runs.
- **More than one font family in a book** (a sans heading, a monospace code block): designed in 11.1 (Stage B).
- **From 2c-2 (2026-10-15):**
  - **Inline sizes:** a `<small>` or `<sup>` inside a paragraph is still the block's face scaled. Native per-word
    sizes need a font ID per word in the section format.
  - **Wide glyphs at the largest sizes:** a glyph wider than the 4 KB output region (ligature code points, DŽ) is
    drawn as missing. Revisit with GSUB ligatures (2c-3).
  - **Layout prefetches the regular style only,** so a bold native heading also renders its regular glyphs, unused.
    Pass the styles the text needs.
  - **The review's deferred minors:**
    - `checkLock` does not cover the resolver's stand-in insert;
    - no host test of the manager's drop and unload order;
    - a refused size claim is cached even after a transient read error;
    - the full-table branch ignores a failed wipe-word write;
    - no power-cut sweep covers "wipe pending but still writing";
    - the ID window could move between sessions if a registered ID falls in it (about 2^-23);
    - faces under about 17 px resample a residual under half a pixel;
    - stale comments.
- **From 2c-3 (2026-10-16):**
  - **Ligature glyphs are checked against `numGlyphs` only.** A ligature glyph the rasterizer cannot draw (a
    point-matched composite, Apple scaled offsets) makes every fi draw as missing. None of about 30 fonts checked
    has one. Fix: test-load each style's ligature glyphs at family load and drop the pairs that fail.
  - **A font whose GPOS kerns only another script** (Leelawadee: `thai` only) loses its legacy table's Latin
    kerning. Fix: when GPOS reports a left glyph uncovered, ask the legacy table.
  - **The union of scripts** differs from a shaper for fonts that split kern lookups by script (one Calibri pair).
  - **Wide glyphs at the largest sizes** (from 2c-2) still apply to ligature glyphs.
  - **`SECTION_FILE_VERSION` 83 is redundant for TTF families** (their font ID changes with `kRenderVersion`).
    It was kept, so every book is laid out once more after the update, built-in fonts included.
  - **`FontBytes::data()` for SD mode (2c-4):** the glue adds file offsets to it, so it must stay null unless the
    whole file is addressable at it.
  - **Small costs:** a 64-bit division per stored pair (`int32` suffices); the `checkLock` message says "glyph
    rendered" for a kerning miss.
  - **`KernTable`:** stops at a subtable whose pair count overruns the table (FreeType clamps and carries on),
    continues past a subtable shorter than 14 bytes (FreeType stops), keeps 16 subtables (FreeType 32).
  - **`kMaxRanges` (8,192)** also caps format-1 Coverage and ClassDef, which are not range lists: glyphs past entry
    8,192 of a huge font lose kerning and ligatures. Unchanged from before.
  - **Test gaps:** SDK: a required feature index, the kern format byte, the 16-subtable cap, version 0, a
    `hasKern` assert on the fixture; app: the legacy fallback when GPOS is present, `checkLock` on a kerning miss,
    positive rounding.
  - **Doc and style nits:** `KernTable.h` "any subtable" means any counted subtable, and one of its sentences is a
    fragment; `OpenType.h`'s header names two users, now three; `run.sh` keeps a stale "GSUB feature branch"
    comment; the SDK's `GposKerningTest.cpp` has a few clang-format deviations.

### 11.1 Stage B: more than one font family per book (designed; phase 1 built, 11.1.1; phase 2 built, 11.1.2)

What #3934 calls "Ignore Body Font", and what most books with embedded fonts actually use them for: the body in
one family, and a few elements set apart in others (a display face for headings and chapter numbers, a script
for a dedication, a monospace for code).

**Layout: a family per block, then per word.**
- **Per block first.** A heading, a `<pre>`, a blockquote or a centred dedication in another family is a whole
  block. `TextBlock` already carries a block font ID (`headingFontId`, which native sizes fill with the family's
  `base | px` ID). The parser resolves the block's `font-family` (already in `CssStyle`) to a family, and the
  family plus the block's size to that ID. No section format change: the ID is stable across sessions, as native
  sizes are. Covers most real uses.
- **Per word second.** An inline `<code>`, a small-caps span in a second family, an accent word: a per-word font
  slot, one byte indexing a per-block table of up to 8 `{fontId, scale}` (#3934's `wordSizeSlots` and
  `MAX_WORD_FONTS`, our `sizePct` byte's sibling). Only blocks that mix families pay the byte. Bumps
  `SECTION_FILE_VERSION`.
- **Generic families** map onto the built-in faces (`serif` Bookerly, `sans-serif` Noto Sans, `monospace` the
  reader's font until a monospace face is built in): the smallest useful step, which needs no store change.

**The store: one store, a face table instead of a size table.** A second store is not an option on the C3: the
index and the log stay mapped while a family is loaded (2 + 17 pages in the 32-page example of 4.1), and two of
them would not fit the about 31 free MMU pages beside the app's 96 (97 with Stage A; memory note on the C3 MMU
window). One store,
then, holding a family set:
- **Glyph key unchanged.** Code point 21 bits, style 2, index 5; bits 28-31 stay zero, so the erased value and the
  log scan's plausibility check stay as they are. The 5-bit index stops meaning "size" and means "face": a
  state-sector entry becomes `{u32 family << 24 | ppem 26.6, u32 claimed}` (ppem stays under 2^14 at 120 px).
  The 32 entries are shared by every family; a book's body takes 5-10 sizes and an accent family 2-4, so 32 still
  covers a book. A full table already wipes at the next book open (4.6).
- **Pair index:** the value's high half carries `family << 2 | style` instead of the style alone.
- **Header:** `kMaxStyles` 4 -> 8 style blocks (two families of four, or a body of four and four one-style accent
  families); `StyleInfo` is 148 B, so the header grows to about 1.3 KB, still one sector. The identity becomes the
  set's: FNV-1a over the families' identities in a fixed order. Any change installs the set again, as a family
  change does now. `kFormatVersion` 2.
- **Mapping:** flash mode maps one style block at a time already; that stays, so the page budget is unchanged
  (the largest style block, not the sum). Store hits need no style block mapped, only the index and the log. A
  miss, a kerning lookup or a cmap lookup in another family remaps (about 8 pages), so prefetch must batch per
  (family, style) (the open "group a page's misses by style" item of 8.1). In SD mode (2c-4) only the copied
  tables are mapped (about 1 page for a Latin subset), so several families' tables could stay mapped at once:
  Stage B is cheaper on top of 2c-4.
- **Partition space:** the font area is about 3.3 MB; embedded families are subsets (50-300 KB a style), so a
  body plus two accent families fits where one full Latin family (1.8 MB) does today.

**Engine and memory.**
- One `TtfFont` per family, sharing one scratch block (one render at a time, under `RenderLock`; 5.3): 6-10 KB
  once, not per family, which the C3 could not afford. Per family: about 0.5 KB per face in use, the GPOS
  subtable list (100-200 B a style) and the ligature tables. A family's native-size window
  (`pickNativeBase`) per family, all served by the one resolver.
- `SdCardFontManager` holds a family set instead of one family, the body (the reader's or the book's) and the
  accent families; `ttfNativeSizes` per family.

**Modes, against #3934's.** Publisher Fonts, Stage A's on/off toggle, becomes an enum of four; the stored numbers
keep Stage A's:
- Off (0): nothing from the book (#3934 Off).
- Body Text (1, Stage A's "on"): the book's body family as the book's font; other families stay the body's.
- Accents (#3934's default "Ignore Body Font"): the reader's font for the body, the book's other families where
  the book uses them. With a built-in reader font the store holds only the accent families, so one accent family
  needs no multi-family store at all: a first step that also needs the manager to load a TTF family that is not
  the body font.
- All Text (#3934 "All Text"): Body Text plus Accents.
The body family is found as in Stage A (`detectBodyFamily`), once per book; "Accents" compares each element's
family against it, as #3934 does per word.

**Effort (estimate):** generic-family mapping and per-block family resolution 2-3 days; the multi-family store
(face table, header, pair index, set identity, power-cut sweeps for the new format) 4-6 days; the manager's family
set and shared scratch 2-3 days; per-word slots 2 days; extraction of several families and the two new modes 1-2
days; device validation on the X3 and X4 2-3 days. About three weeks in all, and an estimated 8-12 KB of flash on
top of Stage A's, which the C3's 96.8 % has to find room for (the 20 pt built-in faces' 505 KB, section 12, are the
obvious source).

**Stage B is delivered in three phases**, each its own PR: (1) per-block families, generic families on the built-in
faces, the four modes, and one accent family beside a built-in reader font (no store or section format change); (2)
the multi-family store and the manager's family set; (3) per-word font slots, and several families extracted.

#### 11.1.1 Phase 1, as built (2026-10-18, branch `feat/book-fonts-stage-b`)

- **CSS (`CSS_CACHE_VERSION` 29):** `CssStyle::fontFamily` keeps the list's first family as an identity
  (`fontFamilyHash`, whose low two bits are now always 0) and, in those two bits, the list's first generic serif (1)
  or sans-serif (2) (`CssFontFamily`). `font-family: Gill Sans, Helvetica, sans-serif` is "Gill Sans, else
  sans-serif"; `serif` alone is identity 0 with the serif bit. Monospace, cursive, fantasy and the keywords stay 0,
  the reader's font.
- **The probe (`BookFonts::detectFamilies`, `kVersion` 4):** families compare by identity, so Stage A's body family is
  unchanged (its goldens and tests pass as they were); the body's value keeps the generic most of its text names. The
  same pass counts blocks (p, div, li, dd, blockquote, pre, h1-h6) by family: a block's text's family when all of it
  is in one, else its own. The accent family is the embedded family the most blocks are set in, other than the body's,
  that has a regular face; it is copied like the body family, to `fonts/accent/<style>.ttf`. A book without
  `@font-face` rules is probed too (its body's generic matters), once, behind the open's popup. `fonts.bin` holds the
  body's font-family, the accent identity and both names (docs/file-formats.md).
- **Layout (`BlockFontFamilies`, `ChapterHtmlSlimParser::resolveBlockFont`):** the reader hands the build a table of
  at most two families: key (an identity or a generic), the family's face at the body size, and its sizes (built-in
  rungs relative to the body size, or a TTF family's native window). The parser keeps font-family on its style stack
  while the table has a family, tracks each block's words (all in one family, or several) and resolves the block to
  its words' family, else to the family of the element that opened it. That family's face at the block's size becomes
  the block font ID (`headingFontId`), body size included, outside the one-auxiliary-face budget. A block in the body
  family, in monospace, or in a family neither in the table nor naming a generic it has, keeps the body font. The
  table rides `Section::BuildParams` by pointer: by value it made every frame that copies the params larger than the
  compressed instructions reach, about 600 B of flash. It joins the property hash only when it has a family and the
  book's style is in use, so Off and Body Text keep their variants and their layout (every golden unchanged). No
  section format change.
- **Modes (`publisherFonts`, `CrossPointSettings::PUBLISHER_FONTS`):** Off 0, Body Text 1 (Stage A), Accents 2, All
  Text 3, the labels in 24 languages. `SdCardFontSystem::selectBookFamily(cachePath, mode, accentAllowed)` picks the
  book family: the body family in Body Text and All Text, else the accent family in Accents and All Text when the
  reader's font is built in (`accentAllowed`). The reader (`buildBookFontFamilies`) maps serif to Bookerly and
  sans-serif to Noto Sans at the body's size through the rung APIs, unless the body is in that built-in family or
  names that generic, and adds the accent family with its native sizes when it is loaded.
- **What waits for phase 2:** two TrueType families at once. Beside a reader font on the card (Accents; a TTF or a `.cpfont` family: they share the font area), or beside the book's
  TTF body (All Text), the accent family is not loaded ("Accent font ... left out" in the log), and a block in it
  takes the generic its CSS names, as a browser does when a font fails to load. So All Text is, for now, Body Text
  plus the generic mapping.
- **Cost, `env:default` (C3):** flash +3,572 B against the Stage A head 652ff0c6e (CSS and probe +1,030, layout +710,
  reader and modes +1,832, of which the labels about 0.5 KB); +3,606 B (5,834,207 B) against master c6b3ecf33 after
  the rebase (#498 dropped the 20 pt faces). Static RAM +24 B. No MMU page boundary crossed against either base. Heap:
  the parser's table copy (about 190 B) during a build; the probe's tables during `prepare` grow from 0.7 to about
  1.3 KB.
- **X4 test (2026-10-18, serial test API, reader font Bookerly 14 pt):** `/test/families_test.epub` (the corpus book
  with Ubuntu as Book Body, Inter as Book Display, Noto Sans as Book Script) in each mode. Off: all Bookerly. Body
  Text: all Ubuntu (`font=book:Book Body`), as Stage A. Accents: body Bookerly; the chapter title, dedication, the
  paragraph that is one span, the blockquote and the heading set in a span in Inter (`book:Book Display (accents)`,
  installed in about 5 s); the generic sans-serif heading and the Helvetica paragraph in Noto Sans; the mixed
  paragraph, `pre`, Georgia and Book Script in Bookerly. All Text: body Ubuntu; Book Display blocks on Noto Sans
  through their sans-serif fallback; the span heading (Book Display without a fallback) in Ubuntu. Accents with
  Bitter (`.cpfont`) as the reader font: "Accent font Book Display left out", Book Display blocks and the sans heading
  in Noto Sans, the body in Bitter. All as the goldens say. Heap in the book: Off 68.5-69.5 KB free, Body Text
  56 KB (largest block 23.5-36.9 KB), Accents with the accent family loaded 53-55 KB (27.6-36.9 KB); at Home after
  each mode 76.4-76.6 KB, unchanged across the switches (master, same book: 70.5 KB in it, 77.2 KB at Home). Found
  on the way: the repository's Inter-Bold.ttf has no bold bit in `head.macStyle` (its weight, 700, is only in OS/2),
  so the registry filed it as a second regular and the accent family installed one style: its headings drew regular.
  A book font with the same flaw lost its bold the same way. Fixed on the branch: `ttf::assignStyles` lets a file of
  weight 600 or more that declares regular (italic) serve bold (bold italic), unless it serves its own style.
- **Tests:** `FontFamilyLayoutTest`, the `BookFontsTest` accent cases, the generic cases of
  `CssParserFonts`, and the corpus book `test_font_families.epub` (generator `test/epubs/make_test_font_families.py`,
  which with `--fonts` also writes the device copy with Ubuntu, Inter and Noto Sans faces) with goldens for the
  default (Off, Body Text), `_accents` and `_alltext`.

**For phase 2, from what phase 1's code showed:**
- `SdCardFontManager` assumes one TTF family throughout: one `TtfFont`, one native window (`ttfNativeBase_`), one
  `loadedFamilyName_`, and `getFontId(name, pt)`, `ttfNativeSizes(id)` and `setTtfBodySize` answer for it alone. A
  family set needs a window per family (`pickNativeBase` per identity, already salted against collisions), the resolver
  dispatching on the window an ID falls in, and a body face per family. `resolveSdCardFontId(name, pt)` can stay as is:
  the reader asks by name.
- `SdCardFontSystem` keeps one `bookFamily_` with a role (body or accent); phase 2 needs both at once, and
  `ensureLoaded` a list of families to load. `buildBookFontFamilies` then adds the accent entry beside a TTF body or
  reader font unchanged; `BlockFontFamilies::kMaxFamilies` goes from 2 to 3 (both generics and the accent beside a TTF
  reader font). Keep it a pointer in `BuildParams`.
- A `.cpfont` reader font occupies the same font area (`FlashFontPartition`, 4.1), so a store of several TTF
  families does not by itself put an accent family beside it: that needs the `.cpfont` read from SD while the store
  holds the accent, or the accent in SD mode (2c-4). Phase 1 leaves the accent out beside any font on the card.
- Phase 3 (per word): the parser already knows each word's family (`effectiveFontFamily_`, `noteWordFamily`), so a
  per-word slot is `BlockFontFamilies::find` per word into #3934's per-block table; and the probe's block tally holds
  every family, so a second accent family is a change to `accent()` alone.
- **A Stage A edge, not fixed here:** a book family is keyed by its declared name (`book:<name>`). Two books whose
  families share a name, opened reader to reader without the font being unloaded in between, keep the first book's
  TTF family: `SdCardFontSystem::ensureLoaded` matches the loaded family by name and only changes its size. Key book
  families by book (cache path or identity) when the manager learns several families. (Fixed in phase 2, 11.1.2.)

#### 11.1.2 Phase 2, as built (2026-10-18, branch `feat/book-fonts-stage-b2`)

- **The store (`kFormatVersion` 2, `GlyphStoreFormat.h`, docs/file-formats.md):** the size table is a face table:
  32 entries `{u32 family << 24 | ppem 26.6, u32 claimed}` (`faceOf`), shared by every family of the set, claimed by
  `FlashGlyphStore::faceIndex(family, ppem)`; the glyph key's 5-bit index names the face, so the key format, the
  erased-value guarantee and the log scan are unchanged. A pair slot's value carries `pairStyle(family, style) =
  family << 2 | style` in its high half (`findPair`/`storePair` take the family). `kMaxStyles` 8 (the header is
  1,240 B, one sector). A family is its index in the set; family 0's faces and pair styles equal version 1's sizes and
  styles. A version-1 header reads as not installed, so the first load installs over it, erasing only what holds data.
  The planner was already right for a set: it counts the largest style block, not their sum.
- **Set identity (`ttf::setIdentity`):** FNV-1a, four lanes, over the count and the families' identities in the set's
  order. Another family, order or count installs the set again: a family's index is in every key it stored.
- **Engine (`TtfFontSet`, `TtfFont.h`):** the set owns what is one per store: the `FlashGlyphStore`, the rasterizer
  and its one scratch block (planned over every family's styles: the larger family's, not their sum), the ring and its
  8 KB budget, the prefetch list, the failure log, and install, open and wipe. `load(families, count, identity)` plans
  one layout over all present styles and copies them as consecutive style blocks, family by family. A `TtfFont` is one
  family of the set: its styles (face, kerning, ligatures, uncovered-left bits), body size, native sizes and stats,
  keyed by its index. Flash mode still maps one style block at a time, every family's; a store hit maps nothing.
  Prefetch was already one family and style per call (one provider per family, size and style, and FontCacheManager
  records text per font ID and style), so a page that mixes families maps each block once per batch; misses and kerning
  misses in another family remap as the spec expected (`knownUncovered` spares most kerning remaps).
  `TtfFontSet::kMaxFamilies` 2.
- **Manager (`SdCardFontManager::loadTtfFamilies`):** opens every family's style files (a heap struct, `TtfSources`,
  so two families' handles stay off the frame), loads them as one set, then registers each family's body face at the
  body size and picks it a native-size window; `pickNativeBase` also steps past another family's window. The resolver
  dispatches on the window an ID falls in (`ttfFamilyOfNativeId`); `ttfNativeSizes`, `getFontId`, `setTtfBodySize`,
  `dropTtfNativeSizes` and `unloadAll` answer for every family. A set that cannot load together (planLayout refuses it
  before any erase, or an accent family that does not read) falls back to the first family alone, logged. The per-family
  record is `TtfFamily {name, identityHash, fontId, nativeBase}`, two of them in the manager.
- **The Stage A edge, fixed:** the manager keeps a loaded set only while it was asked for as the same families from
  the same files (`ttfFamiliesLoaded`, over `SdCardFontFamilyInfo::filesKey`: FNV-1a over the name and the files'
  paths, and a book family's files live in its book's cache). It remembers what was asked, not what loaded, so a set
  that fell back to the body alone is not retried at every open of the same book.
- **`SdCardFontSystem`:** `ensureLoaded(names, count, ...)` takes the families, the body's first; a name not found is
  left out, and a family beside the first only when both are TrueType. The single-name call is a list of one.
  `selectBookFamilies(cachePath, mode, readerFamily)` holds the book's body and accent families at once
  (`bookBody_`, `bookAccent_`; the state file is read twice, once per name); it leaves the accent family out, logged,
  only when the body is drawn in a `.cpfont` reader font. The book open asks for `{body or reader font, accent}`.
- **Layout:** `BlockFontFamilies::kMaxFamilies` 3 (both generics and the accent family beside a TTF reader font),
  still a pointer in `BuildParams`. No section format change. The All Text golden of `test_font_families` now has the
  accent family and equals the Accents golden (the shim draws the two body fonts alike); Off and Body Text unchanged.
- **Cost, `env:default` (C3), against master 83d758e4d (#499 merged; tree equal to phase 1's head):** flash +2,940 B
  (5,837,263 B): the store format +122, `TtfFontSet` +912, the manager's family set +1,482, the reader +424. Static RAM
  +96 B (the manager's family table 64, the second book family 32). No MMU page boundary crossed: code ends 50.7 KB
  into its 42nd page, rodata 21.2 KB short of its 47th, 89 pages in all. Heap: one `TtfFont` per extra family (its
  styles' tables and its faces), no second scratch block; the load's `TtfSources` (about 220 B) only while it runs.
- **Tests:** the glyph store's face table per family and shared, two families' glyphs apart, pairs per family, a set's
  layout and the eight-block limit, a version-1 store installed over, the set identity; the power-cut sweeps with an
  accent face, glyph and pair (session and wipe); `TtfFontSetTest` (Bookerly and Inter in one store: installed once,
  each family's glyphs, native sizes and kerning as its own file gives them, one scratch block, another set or order
  installed again, prefetch per family, a failed write rings both until one wipe, an accent family without a regular
  style refused before any erase); `TtfRegistry` (two books' same-named families told apart); `FontFamilyLayoutTest`
  (three families). Host suite 2830/2830. The manager itself needs the renderer, so its set is exercised on the device.
- **X4 test (2026-10-18, serial test API, Bookerly 14 pt):** `/test/families_test.epub` in every mode, with the
  built-in Bookerly, Bookerly's TTF files as a reader font on the card (BookerlyTT) and Bitter (`.cpfont`).
  - Off: all Bookerly.
  - Body Text: all Ubuntu.
  - Accents with the built-in font: body Bookerly, the Book Display blocks in Inter (Bold headings), the generic sans
    heading in Noto Sans.
  - **Accents with BookerlyTT:** body BookerlyTT, Book Display blocks Inter: "2 families installed: 6 styles, log
    589824 B", about 17 s for the install.
  - **All Text:** body Ubuntu, Book Display blocks Inter (phase 1: Noto Sans), "2 families installed: 4 styles".
  - Accents with Bitter: "Accent font Book Display left out: the reader's font is a .cpfont family", as phase 1.
  - **Two books whose body families share a name** (`families_test2.epub`, Bookerly as "Book Body"), reader to reader:
    each in its own font, the store installed again at each switch.
  - Heap in the book, free: Off 67.4-70.0 KB, Body Text 55.9-57.0 KB, Accents 52.6-55.2 KB, All Text 52.3-54.8 KB,
    Accents beside BookerlyTT 52.8-55.4 KB, All Text with BookerlyTT 54.0-55.3 KB, Accents with Bitter 63.4-65.9 KB
    (phase 1 alike); at Home 75.8-76.4 KB.
  - Found on the way, older than Stage B: `ActivityManager::goToReader` loads the next book's fonts before the reader
    it replaces has exited, so that reader's look-ahead build tick ran against unloaded font IDs ("Font ... not
    found", 27 lines; the build was dropped at its exit, nothing cached). Any reader-to-reader switch that changes
    the font does this; the same-name switch now does too, since it now changes the font.
- **What is left, and phase 3:**
  - A `.cpfont` reader font still has the font area to itself: the accent family beside it needs the `.cpfont` read
    from SD while the store holds the accent, or the accent in SD mode (2c-4).
  - Any change of the set installs every family again, the reader's font included: moving between a book with an
    accent family and one without, or between two books' accent families, beside a TTF reader font, copies the reader
    font each time (about 1.8 MB for Bookerly). An install that keeps a matching leading family's blocks (its face
    entries and log records stay valid if its index does) would spare that; the identity would then be per family.
  - Per-word font slots (`SECTION_FILE_VERSION` bump): the parser knows each word's family; #3934's per-block table of
    up to 8 `{fontId, scale}` slots and a slot byte per word.
  - Several families extracted: the probe's block tally holds every family, so more accent families are `accent()`
    returning a list, `BookFonts` copying each (`fonts/accent<n>/`), and `TtfFontSet::kMaxFamilies`,
    `SdCardFontManager::kMaxTtfFamilies` and `BlockFontFamilies::kMaxFamilies` raised; the store already takes eight
    style blocks and the face table is shared. The fallback would then drop families from the end, one at a time.

## 12. Side findings (separate from this work)

- **Possible null dereference (unverified, code reading only).** For a `.cpfont` loaded from SD (not
  mmap), `unloadMetadata()` sets `fullIntervals` to null before the section build, but
  `findGlobalGlyphIndex` indexes it with no null check during layout prewarm.
- **Stale comments:** the `SdCardFont::loadFromMmap` header comment says "no heap allocation", and the
  `prewarmStyle` mmap comment says bitmaps are not in the mapping. Both are wrong.
- **Performance:** a full prewarm reads glyph records from SD even for mmap fonts, and `onGlyphMiss` has
  no mmap branch.
- **SD fonts kern only when drawing,** so layout widths and drawn widths differ by the kerning.
- **The end-of-book screen cannot make the cover thumbnail while a TTF family is loaded** (2c-3 X3 run): the JPEG
  decoder needs 40,960 B and found 39,684, because the family's 10 KB scratch is still resident. The thumbnail is
  marked transient and made later. 2c-3 adds about 0.7 KB per family; it failed under 2c-2 too.
- **The loop task's stack is 16 KB, not 8 KB.** FreeInkUI overrides `getArduinoLoopTaskStackSize`.
  The comment in `src/main.cpp` above the loop-task high-water log still says 8 KB.
- **SDK:**
  - The `TtfFont.cpp` comment says stb allocates nothing; in fact it allocates a 56 KB block per glyph
    on RV32 and drops edges silently when that fails.
  - stb's GPOS kerning skips Extension (type 9) lookups.
- **The scaled-glyph mask cache overflows on anti-aliased pages** (X3 bench, 2026-10-15; table in the 2c-1
  plan's "Measured" section). A resampled glyph needs up to three masks (BW, LSB, MSB), so a page with 30
  distinct glyphs needs 90 entries against `SCALED_GLYPH_MAX_ENTRIES` = 80. The cache resets wholesale and
  every page resamples again: a 14 → 20 pt page took 274 ms resampled against 80 ms native. This hits
  built-in sizes 22-26 pt (resampled from the 20 pt face), `.cpfont` alias sizes and CSS-scaled body text
  today, not only TTF.
- **Follow-up, at the user's request (2026-10-15): drop the largest pre-rendered built-in size and resample
  it too.**
  - The 20 pt faces are Bookerly 262 KB plus Noto Sans 243 KB: 505 KB of app flash, which is at 96.2%.
    Without them 20-26 pt would be resampled from 18 pt (453 KB, kept), at scales of 1.11 to 1.44.
  - Before deciding, size the mask cache for AA pages (at least three entries per distinct glyph, and an
    arena for 26 pt masks). Then measure page turns at 20-26 pt from 18 pt against today's 20 pt masters,
    and compare the quality on the X3 and X4.
  - The cost sits on body text: at those sizes every glyph of the page is resampled, about 1 ms each at
    24-26 pt (`SCALED_GLYPH_ARENA_BYTES_SYNTH`).
  - The home for this is `docs/future_work/font-flash.md` once it is decided.

## Appendix A. Step 0 measurements (X3, 160 MHz, 2026-10-13)

Measured with the `bench_ttf` sketch on branch `bench/ttf-step0` (`bench/ttf/README.md` says how to
re-run it). Three runs agreed within 1%. Renderer: the integer loader plus standalone `ftgrays`,
rendering through the span callback straight to 2-bit.

**Render, µs per glyph** (29 px/em = 14 pt / 54 px/em = 26 pt). The cold pass, which keeps the render
code cached, is about equal to the warm one. The last row empties the 16 KB flash cache before each
glyph.

| Font | 2 KB pool | 4 KB pool | 8 KB pool |
| --- | --- | --- | --- |
| Bookerly | 469 / 769 | 429 / 691 | 424 / 628 |
| Noto Sans | 369 / 643 | 335 / 559 | 335 / 512 |
| CJK (Microsoft YaHei subset) | 1,327 / 2,297 | 1,185 / 1,896 | 833 / 1,713 |
| Flash cache emptied, 29 px, Bookerly / CJK | 738 / 1,585 | 696 / 1,442 | 690 / 1,087 |

- The output is byte-identical across pool sizes (host span check).
- An emptied flash cache adds about 0.25-0.27 ms per glyph.
- Stack used while rendering with the pool on the stack: 4,064 / 6,128 / 10,224 B for the 2 / 4 / 8 KB
  pool. The loop task's stack is 16 KB (FreeInkUI's `getArduinoLoopTaskStackSize`). Production puts the
  pool in the scratch block (5.3).

**Flash (`spiffs` partition):**
- Erase: 64 KB of written flash 138 ms; 1 MB of mostly blank flash 1.08-1.58 s; a full wipe of a written
  1.2-1.4 MB store about 2.6-3.1 s.
- Program: a 100 B record 297 µs, a 252 B record 601 µs, an 8 B index slot 124 µs.
- Each `esp_partition_write` call has a fixed cost: writing a record's header and bitmap in one call saves
  about 120 µs per glyph.

**Store hit:** 1.5 µs per hash probe through mapped flash, against 1.08 µs for a built-in
`glyph_lookup`.

**Mapping budget:** the reader image leaves **32** free MMU pages (a device boot probe; the 6.27 MB image
uses 96 of 128). The largest mapping of the `spiffs` partition is 32 pages (2 MB). An estimate from the
ELF must take the union of the IROM and DROM page indices, because both buses index the same table;
summing the segments over-counts.

**One page** (1,199 characters, 42 distinct; no kerning on any path):

| Font | built-in `.cpfont` | TTF, first contact | TTF, warm store |
| --- | --- | --- | --- |
| Bookerly | 41.9 ms (16.3 prewarm + 25.6 draw) | 66.8 ms (18.3 render + 25.5 writes) | **21.2 ms** |
| Noto Sans | 34.7 ms | 58.3 ms | **18.4 ms** |

A warm store is about twice as fast as a built-in font, because nothing is decompressed. On first contact
the flash writes cost more than the rendering.

**Kerning, µs per pair** (Bookerly). "Page" is a real page with 736 kernable pairs; "chapter" is 30,000
characters of Moby-Dick with 19,379 pairs. Times include about 0.15-0.19 µs of loop overhead. Every
option returned exactly what the reference reader returns.

| Option | Page | Chapter | Cost |
| --- | --- | --- | --- |
| FreeInkFont `Gpos.cpp` | 137 | 136 | none |
| Pre-resolved GPOS reader (`fastkern.h`) | 37.8 | 37.7 | none |
| Reader + per-left-glyph subtable mask + RAM pair cache | 5.4 cold, 1.4 again | 2.0 (83% hits) | 5 KB RAM, cold every session |
| Class tables compiled at install, read from flash | 3.2 | 3.0 | 1.37 s compile + 0.28 s write, 71 KB flash and a 114 KB transient heap peak, per style |
| **Pairs stored lazily in the flash store (chosen)** | 58.7 first, 0.9 warm | 5.5 first, **0.45 warm** | 229 slot writes for the first page, 397 more over the chapter |

Noto Sans: `Gpos.cpp` 90 µs, the reader 23.0 µs, compiled tables 2.4-2.6 µs, the RAM cache 2.0 µs over
the chapter, the lazy store 0.45 µs warm.

**SD card** (the full 19.7 MB YaHei font as `/bench/cjk.ttf`):
- A random 512 B read costs 4.5 ms; a sorted one 0.35 ms. A backward seek makes SdFat walk the FAT chain
  from the start of the file, so the cost grows with the file size (21.5 ms in a 167 MB file).
- Reading whole glyph records (666 B, 2.3 sectors on average): **4.76 ms per glyph in text order, 0.73 ms
  in file order.**

**Gates** (9.1), from the cache-emptied render figures and the measured writes (about 0.61 ms per Latin
glyph, 0.73 ms per CJK glyph):
- Latin all-in: 0.70 ms render (4 KB pool) + 0.61 ms writes = **1.3 ms**: PASS.
- CJK all-in: 1.44 ms render (4 KB pool) + 0.73 ms SD read in file order + 0.73 ms writes = **2.9 ms**
  (2.5 ms with the 8 KB pool): REVISIT. In text order it is 6.9 ms.
- CJK first chapter, assuming 1,500 new glyphs: 4.3 s (4 KB pool) or 3.8 s (8 KB pool) in file order:
  PASS. In text order about 10.4 s: FAIL. The remedy taken is batching misses in glyph-ID order (3.2,
  5.1), not the metrics-only layout mode.
- Free mapping pages: 32: PASS.

### Stage 2b engine (X3, 160 MHz, 2026-10-14)

Measured while writing the Stage 2b plan, with the production engine (integer loader, vendored `ftgrays`)
built from the plan's code. Device: `bench_ttf_raster` (`bench/ttf_raster_main.cpp`), Bookerly Regular
embedded in the image and read through the flash cache.

**Against FreeType 2.13.3, unhinted (host, every glyph index of all 12 bundled TTFs at 8, 14 and 40 pt):**
- 0 load failures, 0 advance differences, 0 box differences.
- Every glyph without overlap flags is byte-identical to FreeType.
- Glyphs flagged `OVERLAP_SIMPLE` or `OVERLAP_COMPOUND` (Inter, Noto Sans), which FreeType renders 4x4
  oversampled: 0.24-1.30% of pixels differ (spec gate: 3%).
- The only glyph too large for an `EpdGlyph` is Ubuntu's wordmark (U+F200) at 40 pt, 288 px wide.

**Device bench, all 1,867 Bookerly glyphs:**

| Size, pool | avg µs | max µs |
| --- | --- | --- |
| 14 pt (29 px), 2 KB | 512 | 2,548 |
| 14 pt (29 px), 4 KB | 460 | 2,452 |
| 14 pt (29 px), 8 KB | 454 | 1,170 |
| 40 pt (83 px), 4 KB | 1,198 | 5,786 |
| 40 pt (83 px), 8 KB | 1,080 | 4,192 |
| 14 pt, the 95 printable ASCII glyphs, 4 KB | 363 | |

- The output hash equals the host's (`de1aeea7`), and it is the same with every pool size.
- Scratch block 9,964 B, given back in full by `release()`.
- Rendering takes the loop task's stack 80 B below its earlier low: the pool is off the stack.
- Engine code linked into the bench: about 9.2 KB (step 0's prototype: 6.4 KB).
- The review fixes (PR #470) do not change Bookerly's output: the host's golden hash still matches.

**Decision gate: the 64-bit `ftgrays` path (`FT_INT64`), kept (user, 2026-10-14).** The same bench without
the patch:

| Render per glyph | 64-bit (kept) | 32-bit | Cost |
| --- | --- | --- | --- |
| 14 pt, ASCII glyphs | 363 µs | 313 µs | +16% |
| 14 pt, all glyphs, 4 KB pool | 461 µs | 396 µs | +16% |
| 14 pt, all glyphs, 8 KB pool | 454 µs | 391 µs | +16% |
| 40 pt, all glyphs, 4 KB pool | 1,198 µs | 1,050 µs | +14% |

- **Quality.** Without the patch, 24-61 glyphs per Bookerly or Ubuntu face differ from FreeType at 8-14 pt,
  and about 290 at 40 pt. Each differs by one gray level on a single pixel, which is not visible on the
  panel. Neither result is wrong: the 64-bit path is FreeType's current curve flattening, the 32-bit
  path its older one.
- **Testability, the deciding factor.** The 32-bit path's output depends on the width of `long`. The X3 and
  Windows give one result, CI's Linux another; the device hash was `b5645a52` against the host's
  `de1aeea7`. Without the patch, the FreeType gate would need a tolerance and the host/device check could
  not exist.
- **Cost on a page:** about 2 ms on a page's first contact (42 new glyphs x 50 µs), nothing once stored.

**One page with the new engine** (step 0's page: 1,199 characters, 42 distinct glyphs, Bookerly, no
kerning). The TTF columns are estimated from the measurements above. Render: 42 x 363 µs. Writes: 42 x 340 µs
batched (the Stage 2a self-test). Drawing: as step 0.

| | Built-in `.cpfont` | TTF, first contact | TTF, warm store |
| --- | --- | --- | --- |
| Step 0 (prototype, measured) | 41.9 ms (16.3 decompress + 25.6 draw) | 66.8 ms (18.3 render + 25.5 writes + about 23 draw) | 21.2 ms |
| Stage 2b engine (estimated) | 41.9 ms | about 52 ms (15 render + 14 writes + 23 draw) | about 21 ms |

- First contact happens once per glyph, size and family, ever: the store survives reboots, and reopening it
  costs about 30 ms at family load. New glyphs are mostly rendered during section builds, so the cost shows
  as indexing time rather than as page-turn latency.
- Kerning is extra:
  - warm, about 0.3-0.7 ms per page (0.45-0.9 µs per pair);
  - on the very first page, about 43 ms (58.7 µs per pair), averaging 5.5 µs per pair over a chapter;
  - the pair pre-check obligation in 8.1 targets that first-contact cost.

### Stage 2c-1 (X3, 2026-10-15)

Firmware `d663f7c69` (`feat/ttf-families`), Bookerly's four TTF files (450 KB each) in
`/.crosspoint/fonts/BookerlyTTF/`, a 16 pt body.

- **Discovery:** probing a four-file family took about 19 ms at boot.
- **Install:** 8.7 s behind the popup on a blank font area, about 13 s when a `.cpfont` cache had to be erased
  first. Log 1,245,184 B, 24,576 index slots; scratch block 9,032 B.
- **Pages:** pre-rendering a page from the store took 44-53 ms, against 67-97 ms for the built-in Bookerly on the
  same chapter (16 pt, 42 pages) and 57-61 ms for a `.cpfont` (Bitter). A pre-render cost about 3.6 KB of heap,
  against 3.6-9.9 KB for the built-in and 7.6-7.8 KB for the `.cpfont`.
- **Size changes:** 16 → 8 → 12 → 16 pt each re-laid out the chapter without a reload; with every glyph rendered
  for the first time that took 2.4 s at 8 pt and 2.6 s at 12 pt.
- **Next boot:** the store reopened with 342 glyphs; the session logged "0 glyphs rendered, 15,276 read from the
  store".
- **Stack:** no "Render task stack LOW" (under 1,536 B) through cold chapter builds; no RenderLock warning.
- **Flash and RAM:** +31,180 B of app flash (96.2%), +56 B of static RAM; the engine 9.7 KB, `TtfFont` 5.7 KB,
  the store 5.3 KB, `EspNorFlash` 1.0 KB.
- **Native against resampled sizes:** see the 2c-1 plan's "Measured" section: on anti-aliased pages a native size
  is 2.3-4.6x faster than resampling the body face, which led to 2c-2.

### Stage 2c-2 (X3, 2026-10-15)

Branch `feat/ttf-native-sizes` after the review fixes (`e10513e79`), Bookerly's four TTF files, a 14 pt body.

- **Memory:**
  - scratch block 10,135 B, 1,103 B more than 2c-1's 9,032 B (the output region reaches its 4 KB cap from 64 px);
  - 540 B per native size;
  - app flash +3,310 B over master (96.3%), static RAM +16 B.
- **The drop cap:** placed natively (`scale=1.00`, a 71x77 zone); on the device it is as sharp as the body text.
- **A book with 20 sizes** (paragraphs at 50-300% of the body, h1-h3, a 3-line drop cap):
  - 16 sizes were held, and the rest were drawn by the nearest size held, scaled: 76 and 82 px in the first
    session, 23 and 38 px after a restart that resumed on the last page;
  - the cached layout drew correctly both times, paging forward and back.
- **Heap while reading:** 41-45 KB free with an ordinary book, 34-37 KB with the 20-size book (16 sizes, about
  8.6 KB, plus the ring). A screenshot's 48 KB screen copy failed at that level.
- **The store:**
  - a first session rendered 208 glyphs and read 244,359 from the store;
  - after these sessions the log was at 145 KB of 1.2 MB;
  - the size table did not fill.

### Stage 2c-3 (X3, 2026-10-16)

Branch `feat/ttf-kerning` at the SDK pin `e140f74`, Bookerly's four TTF files, 14 and 16 pt.

- **Install:** the render version bump reinstalled the family once (4 styles, a 1,245,184 B log). At load:
  `regular style: 12 kern subtables, 5 ligatures`. Scratch is unchanged at 10,135 B.
- **What the user saw:** capital pairs (AV, To, Ty, We) tighter, fi and fl drawn as one glyph, bold and italic
  words in running text correct.
- **Pages:**
  - pre-render 47-61 ms a page (2c-1: 44-53 ms);
  - pages served from the store, revisited: 50-61 ms;
  - a first page after a chapter is laid out: 2.1-3.1 s;
  - other full renders 510-690 ms, the display included.
- **Kerning pairs worked out from the font, per session:**
  - 817 for a first chapter at 16 pt;
  - 684 for 20 pages at 14 pt, two new chapters included;
  - 30 for a session of bold and italic pages;
  - 3 when revisiting stored pages.
- **No** RenderLock violations, crashes or stack warnings.
- **Flash:** default image 6,316,919 B (96.4%), x4pro 94.6%.

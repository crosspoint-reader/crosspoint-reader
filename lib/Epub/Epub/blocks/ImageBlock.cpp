#include "ImageBlock.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <cstdlib>
#include <cstring>
#include <new>

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/PxcFormat.h"

#ifdef CROSSPOINT_PSRAM_IMAGE_CACHE
#include <esp_heap_caps.h>
#endif

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height)
    : imagePath(imagePath), srcPath(srcPath), width(width), height(height) {}

void* ImageBlock::extractCtx = nullptr;
ImageBlock::ExtractFn ImageBlock::extractFn = nullptr;

void ImageBlock::setExtractor(void* ctx, ExtractFn fn) {
  extractCtx = ctx;
  extractFn = fn;
}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

std::string getCachePath(const std::string& imagePath) {
  // Replace extension with .pxc (pixel cache)
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + ".pxc";
  }
  return imagePath + ".pxc";
}

bool readValidCacheHeader(HalFile& cacheFile, const int expectedWidth, const int expectedHeight, uint16_t& cachedWidth,
                          uint16_t& cachedHeight) {
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    return false;
  }

  const int widthDiff = abs(cachedWidth - expectedWidth);
  const int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) {
    return false;
  }

  const size_t bytesPerRow = (cachedWidth + 3) / 4;
  const size_t expectedSize = 4 + bytesPerRow * cachedHeight;
  return cacheFile.size() >= expectedSize;
}

// Pages are deserialized afresh on each visit. Keep a bounded, allocation-free
// record so an image that failed renders its placeholder directly for the rest
// of the reader session instead of paying another placeholder refresh and
// decode. The reader clears this on entry so transient memory/storage failures
// are retried.
constexpr size_t MAX_SESSION_IMAGE_FAILURES = 16;
uint64_t failedImageHashes[MAX_SESSION_IMAGE_FAILURES];
size_t failedImageCount = 0;

uint64_t imagePathHash(const std::string& path) {
  uint64_t hash = 14695981039346656037ull;
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool imageFailedThisSession(const std::string& path) {
  const uint64_t hash = imagePathHash(path);
  for (size_t i = 0; i < failedImageCount; i++) {
    if (failedImageHashes[i] == hash) return true;
  }
  return false;
}

void rememberImageFailure(const std::string& path) {
  if (failedImageCount == MAX_SESSION_IMAGE_FAILURES || imageFailedThisSession(path)) return;
  failedImageHashes[failedImageCount++] = imagePathHash(path);
}

// Render the payload straight off SD, a few rows at a time: the fallback for
// every path that could not hold the image in RAM. `cacheFile` must be
// positioned just past the header.
bool renderStreamingFromCache(GfxRenderer& renderer, HalFile& cacheFile, const int x, const int y,
                              const int cachedWidth, const int cachedHeight, const int bytesPerRow) {
  // Read several rows per SD access. A one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat; batching
  // rows into a ~4KB buffer cuts that to ~20 reads per pass without holding the
  // whole image.
  int rowsPerRead = 4096 / bytesPerRow;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > cachedHeight) rowsPerRead = cachedHeight;
  uint8_t* readBuffer = (uint8_t*)malloc((size_t)rowsPerRead * bytesPerRow);
  if (!readBuffer) {
    // Fall back to a single-row buffer under memory pressure.
    rowsPerRead = 1;
    readBuffer = (uint8_t*)malloc(bytesPerRow);
  }
  if (!readBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = 0; row < cachedHeight; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (cachedHeight - row < rowsPerRead) ? (cachedHeight - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRow;
      if (cacheFile.read(readBuffer, bytes) != static_cast<int>(bytes)) {
        LOG_ERR("IMG", "Cache read error at row %d", row);
        free(readBuffer);
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* rowBuffer = readBuffer + (size_t)bufferRow * bytesPerRow;
    bufferRow++;

    const int destY = y + row;
    pw.beginRow(destY);
    // On a grayscale strip pass only a narrow column window of the image is in
    // the active band; skip the rest instead of unpacking+clipping every pixel.
    int colStart, colEnd;
    pw.bandColRange(x, cachedWidth, colStart, colEnd);
    for (int col = colStart; col < colEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;

      pw.writePixel(x + col, pixelValue);
    }
  }

  free(readBuffer);
  LOG_DBG("IMG", "Cache render complete");
  return true;
}

#ifdef CROSSPOINT_PSRAM_IMAGE_CACHE
// --- Whole-image PSRAM render cache ------------------------------------------
// Replaces the single per-page-render internal-RAM slot below. The tiled
// grayscale flow re-renders an image page once for the BW double-refresh and
// again for every band of both gray planes, and each pass re-read the whole
// .pxc off SD (~100 ms for a full-page image, ~13 passes). Column clipping
// cannot reduce that traffic: the row stride (~100 B) is smaller than an SD
// sector, so every sector is touched regardless of the band window.
//
// PSRAM makes the obvious fix affordable. Each slot is one contiguous
// heap_caps_malloc(MALLOC_CAP_SPIRAM) block holding a whole .pxc image (header
// + payload), keyed by the hash of its cache path, and the set of slots
// survives page turns. Three things follow from that:
//  * every pass of every image on a page hits RAM, not just the first image
//    (the internal-RAM design could only afford ONE resident image, so a
//    two-image page re-streamed the second one ~13 times);
//  * a back-turn, or any re-visit while the image is still resident, costs no
//    SD access at all;
//  * a decode can hand its freshly built payload straight over
//    (adoptPxcImage), so the passes after a first view never read back
//    the file the decode just wrote.
//
// Bounded: 8 slots x at most 96,004 B is ~750 KB of the 8 MB PSRAM, held for as
// long as the reader is open (clearPxcLru gives it back on the way out).
// Eviction is LRU, so a new book simply displaces the previous one's images.
// Failure to allocate is not an error anywhere: the slot stays empty and the
// caller falls back to the unchanged streaming path.
//
// Threading: the cache has no lock of its own. Its invariant is that every
// access happens on a task holding the RenderLock -- which is true of every
// render and of every decode, both of which run under it.
constexpr size_t PXC_LRU_SLOTS = 8;

struct PxcLruSlot {
  uint8_t* image;   // PXC_HEADER_BYTES + payload, one PSRAM allocation
  size_t capacity;  // bytes allocated at `image`
  uint64_t hash;    // imagePathHash(cachePath); 0 (with image == nullptr) = empty
  uint16_t width;
  uint16_t height;
  uint32_t lastUse;
};

PxcLruSlot pxcLru[PXC_LRU_SLOTS] = {};
uint32_t pxcLruClock = 0;

void pxcLruDrop(PxcLruSlot& slot) {
  free(slot.image);  // heap_caps_malloc'd memory is free()-compatible
  slot = {};
}

// The slot this image should occupy: its own entry if it already has one, else
// an empty slot, else the least recently used one. Never null; the returned
// slot may still hold another image's payload (the caller replaces it).
PxcLruSlot& pxcLruSlotFor(const uint64_t hash) {
  PxcLruSlot* victim = &pxcLru[0];
  for (auto& slot : pxcLru) {
    if (slot.image && slot.hash == hash) return slot;
    // An empty slot has lastUse 0, so it always wins this comparison.
    if (slot.lastUse < victim->lastUse) victim = &slot;
  }
  return *victim;
}

PxcLruSlot* pxcLruFind(const uint64_t hash) {
  for (auto& slot : pxcLru) {
    if (slot.image && slot.hash == hash) {
      slot.lastUse = ++pxcLruClock;
      return &slot;
    }
  }
  return nullptr;
}

// True when this image's payload is already resident at the size the caller
// expects. A peek, deliberately: asking "would this still need decoding?" is
// not a use, so it leaves lastUse alone and cannot reorder the LRU.
bool pxcLruHasImage(const std::string& cachePath, const int expectedWidth, const int expectedHeight) {
  const uint64_t hash = imagePathHash(cachePath);
  for (const auto& slot : pxcLru) {
    if (!slot.image || slot.hash != hash) continue;
    // Same +/-1 tolerance as the on-disk header check (readValidCacheHeader):
    // re-layout recomputes an image's box without touching its path.
    return abs(slot.width - expectedWidth) <= 1 && abs(slot.height - expectedHeight) <= 1;
  }
  return false;
}

// Point `slot` at a `bytes`-sized PSRAM block for `hash`, reusing the block it
// already owns when that is big enough. False leaves the slot empty.
bool pxcLruReserve(PxcLruSlot& slot, const uint64_t hash, const size_t bytes) {
  if (slot.capacity < bytes) {
    pxcLruDrop(slot);
    auto* block = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!block) return false;
    slot.image = block;
    slot.capacity = bytes;
  }
  slot.hash = hash;
  // The payload for `hash` has not been read in yet, and a reserve that reuses
  // an oversized block keeps the previous occupant's bytes. Zero the dimensions
  // so the slot never advertises (new hash, old size) -- pxcLruHasImage and
  // renderFromCache both trust that pair.
  slot.width = 0;
  slot.height = 0;
  slot.lastUse = ++pxcLruClock;
  return true;
}

void renderRowsFromPxcSlot(GfxRenderer& renderer, const PxcLruSlot& slot, const int x, const int y) {
  const uint8_t* pixels = slot.image + PXC_HEADER_BYTES;
  const int bytesPerRow = (slot.width + 3) / 4;

  DirectPixelWriter pw;
  pw.init(renderer);

  for (int row = 0; row < slot.height; row++) {
    const uint8_t* rowBuffer = pixels + static_cast<size_t>(row) * bytesPerRow;
    pw.beginRow(y + row);
    int colStart, colEnd;
    pw.bandColRange(x, slot.width, colStart, colEnd);
    for (int col = colStart; col < colEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      const uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;
      pw.writePixel(x + col, pixelValue);
    }
  }
}

// cacheFile is positioned just past the header. Returns the filled slot, or
// null when the image is too big for a slot, PSRAM is exhausted, or the read
// came up short -- all of which fall back to streaming.
const PxcLruSlot* loadPxcSlot(const uint64_t cacheHash, HalFile& cacheFile, const uint16_t cachedWidth,
                              const uint16_t cachedHeight, const int bytesPerRow) {
  const size_t payload = static_cast<size_t>(bytesPerRow) * cachedHeight;
  if (payload == 0 || payload > PXC_MAX_PAYLOAD_BYTES) return nullptr;
  const size_t total = PXC_HEADER_BYTES + payload;

  PxcLruSlot& slot = pxcLruSlotFor(cacheHash);
  if (!pxcLruReserve(slot, cacheHash, total)) return nullptr;

  // Short reads are legal (see the book.bin mirror), so loop to completion.
  size_t got = 0;
  while (got < payload) {
    const int n = cacheFile.read(slot.image + PXC_HEADER_BYTES + got, payload - got);
    if (n <= 0) break;
    got += static_cast<size_t>(n);
  }
  if (got != payload) {
    pxcLruDrop(slot);
    return nullptr;
  }

  memcpy(slot.image, &cachedWidth, 2);
  memcpy(slot.image + 2, &cachedHeight, 2);
  slot.width = cachedWidth;
  slot.height = cachedHeight;
  return &slot;
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, const int x, const int y,
                     const int expectedWidth, const int expectedHeight) {
  const uint64_t cacheHash = imagePathHash(cachePath);
  if (PxcLruSlot* slot = pxcLruFind(cacheHash)) {
    // Re-layout (font size, orientation, margins) recomputes an image's box
    // without touching its path, so a resident payload can name the wrong size.
    // Same +/-1 tolerance as the on-disk header check; a mismatch drops the
    // entry and falls through to the file, which is itself about to be rejected
    // and re-decoded.
    if (abs(slot->width - expectedWidth) <= 1 && abs(slot->height - expectedHeight) <= 1) {
      renderRowsFromPxcSlot(renderer, *slot, x, y);
      return true;
    }
    pxcLruDrop(*slot);
  }

  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (!readValidCacheHeader(cacheFile, expectedWidth, expectedHeight, cachedWidth, cachedHeight)) {
    LOG_ERR("IMG", "Invalid image cache: %s", cachePath.c_str());
    return false;
  }

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);

  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte

  // First pass after a miss: pull the payload into a slot so this pass and
  // every later one (this page render, and every re-visit until the slot is
  // evicted) skip SD entirely.
  if (const PxcLruSlot* slot = loadPxcSlot(cacheHash, cacheFile, cachedWidth, cachedHeight, bytesPerRow)) {
    renderRowsFromPxcSlot(renderer, *slot, x, y);
    LOG_DBG("IMG", "Cache render complete (payload now in PSRAM)");
    return true;
  }

  // Streaming fallback (no slot). A failed slot load may have consumed part of
  // the payload; rewind to just past the header.
  cacheFile.seek(4);
  return renderStreamingFromCache(renderer, cacheFile, x, y, cachedWidth, cachedHeight, bytesPerRow);
}

#else  // !CROSSPOINT_PSRAM_IMAGE_CACHE

// --- Per-page-render RAM slot for the pixel cache ----------------------------
// The tiled grayscale flow re-renders an image page once for the BW
// double-refresh and again for every band of both gray planes, and each pass
// re-read the whole .pxc off SD (~100 ms for a full-page image, ~13 passes).
// Column clipping cannot reduce the SD traffic: the row stride (~100 B) is
// smaller than an SD sector, so every sector is touched regardless of the band
// window. Instead the first pass loads the payload into RAM and later passes
// render from it. Chunked allocation because a single full-image block (up to
// 96 KB) rarely fits the fragmented mid-render heap; each chunk is heap-gated
// and any failure falls back to the streaming path unchanged. The reader
// releases the slot when the page render completes, so nothing stays resident
// across page turns.
constexpr size_t PXC_CHUNK_SHIFT = 14;  // 16 KB chunks
constexpr size_t PXC_CHUNK_SIZE = 1u << PXC_CHUNK_SHIFT;
constexpr size_t PXC_MAX_CHUNKS = 6;  // 96 KB: a full-screen 2bpp image
constexpr size_t PXC_HEAP_RESERVE = 24 * 1024;
constexpr size_t PXC_MAX_ALLOC_RESERVE = 8 * 1024;
// Rows can straddle a chunk boundary; they are reassembled into a stack
// buffer. (screenWidth + 3) / 4 caps at 200 B for an 800px panel.
constexpr int PXC_MAX_BYTES_PER_ROW = 208;

std::unique_ptr<uint8_t[]> pxcChunks[PXC_MAX_CHUNKS];
uint64_t pxcSlotHash = 0;
uint16_t pxcSlotWidth = 0;
uint16_t pxcSlotHeight = 0;

void releasePxcSlot() {
  for (auto& chunk : pxcChunks) chunk.reset();
  pxcSlotHash = 0;
  pxcSlotWidth = 0;
  pxcSlotHeight = 0;
}

const uint8_t* pxcRowPtr(size_t rowStart, int bytesPerRow, uint8_t* tempRow) {
  const size_t chunk = rowStart >> PXC_CHUNK_SHIFT;
  const size_t offset = rowStart & (PXC_CHUNK_SIZE - 1);
  if (offset + bytesPerRow <= PXC_CHUNK_SIZE) {
    return pxcChunks[chunk].get() + offset;
  }
  const size_t firstPart = PXC_CHUNK_SIZE - offset;
  memcpy(tempRow, pxcChunks[chunk].get() + offset, firstPart);
  memcpy(tempRow + firstPart, pxcChunks[chunk + 1].get(), bytesPerRow - firstPart);
  return tempRow;
}

// cacheFile is positioned just past the header. True when the slot holds the
// full pixel payload for this cache path afterward.
bool loadPxcSlot(uint64_t cacheHash, HalFile& cacheFile, uint16_t cachedWidth, uint16_t cachedHeight, int bytesPerRow) {
  releasePxcSlot();
  if (bytesPerRow > PXC_MAX_BYTES_PER_ROW) {
    return false;
  }
  size_t remaining = (size_t)bytesPerRow * cachedHeight;
  const size_t chunkCount = (remaining + PXC_CHUNK_SIZE - 1) >> PXC_CHUNK_SHIFT;
  if (chunkCount == 0 || chunkCount > PXC_MAX_CHUNKS) {
    return false;
  }
  for (size_t i = 0; i < chunkCount; i++) {
    const size_t want = remaining < PXC_CHUNK_SIZE ? remaining : PXC_CHUNK_SIZE;
    if (ESP.getFreeHeap() < remaining + PXC_HEAP_RESERVE || ESP.getMaxAllocHeap() < want + PXC_MAX_ALLOC_RESERVE) {
      releasePxcSlot();
      return false;
    }
    pxcChunks[i] = makeUniqueNoThrow<uint8_t[]>(want);
    if (!pxcChunks[i] || cacheFile.read(pxcChunks[i].get(), want) != static_cast<int>(want)) {
      releasePxcSlot();
      return false;
    }
    remaining -= want;
  }
  pxcSlotHash = cacheHash;
  pxcSlotWidth = cachedWidth;
  pxcSlotHeight = cachedHeight;
  return true;
}

void renderRowsFromPxcSlot(GfxRenderer& renderer, int x, int y) {
  const int bytesPerRow = (pxcSlotWidth + 3) / 4;
  uint8_t tempRow[PXC_MAX_BYTES_PER_ROW];

  DirectPixelWriter pw;
  pw.init(renderer);

  for (int row = 0; row < pxcSlotHeight; row++) {
    const uint8_t* rowBuffer = pxcRowPtr((size_t)row * bytesPerRow, bytesPerRow, tempRow);
    pw.beginRow(y + row);
    int colStart, colEnd;
    pw.bandColRange(x, pxcSlotWidth, colStart, colEnd);
    for (int col = colStart; col < colEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      const uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;
      pw.writePixel(x + col, pixelValue);
    }
  }
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight) {
  // A later pass of the same page render: the payload is already in RAM, skip
  // the file entirely.
  const uint64_t cacheHash = imagePathHash(cachePath);
  if (pxcSlotHash == cacheHash && pxcSlotWidth != 0) {
    renderRowsFromPxcSlot(renderer, x, y);
    return true;
  }

  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (!readValidCacheHeader(cacheFile, expectedWidth, expectedHeight, cachedWidth, cachedHeight)) {
    LOG_ERR("IMG", "Invalid image cache: %s", cachePath.c_str());
    return false;
  }

  // Use cached dimensions for rendering (they're the actual decoded size)
  expectedWidth = cachedWidth;
  expectedHeight = cachedHeight;

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);

  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte

  // First pass of a page render: try to pull the payload into the RAM slot so
  // the remaining ~12 passes skip SD entirely. Only an EMPTY slot is claimed:
  // the slot lives until the page render completes, so a populated slot with a
  // different hash means another image on this same page owns it. Evicting it
  // here would make 2+ image pages reload each other from SD on every pass
  // (all the SD traffic of streaming plus the slot alloc churn); instead later
  // images take the streaming path below, unchanged from pre-cache behavior.
  if (pxcSlotHash == 0 && loadPxcSlot(cacheHash, cacheFile, cachedWidth, cachedHeight, bytesPerRow)) {
    renderRowsFromPxcSlot(renderer, x, y);
    LOG_DBG("IMG", "Cache render complete (payload now in RAM)");
    return true;
  }

  // Streaming fallback (slot didn't fit). A failed slot load may have consumed
  // part of the payload; rewind to just past the header.
  cacheFile.seek(4);
  return renderStreamingFromCache(renderer, cacheFile, x, y, cachedWidth, cachedHeight, bytesPerRow);
}

#endif  // CROSSPOINT_PSRAM_IMAGE_CACHE

}  // namespace

bool ImageBlock::hasValidCache() const {
  const auto cachePath = getCachePath(imagePath);
#ifdef CROSSPOINT_PSRAM_IMAGE_CACHE
  // A payload already resident in the render cache is as good as the file, and
  // answering from RAM costs no SD access at all -- which is the point, because
  // this runs per image on the page-turn path (renderContents, via
  // Page::hasImagesNeedingDecode). It also means an image whose file was removed
  // underneath a live slot (cache cleanup) stops looking like undecoded work.
  // That caller holds the RenderLock, which is this cache's only
  // synchronization. The file check below remains the fallback for anything not
  // (or no longer) resident.
  if (pxcLruHasImage(cachePath, width, height)) return true;
#endif
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  return readValidCacheHeader(cacheFile, width, height, cachedWidth, cachedHeight);
}

bool ImageBlock::needsDecode() const { return !imageFailedThisSession(imagePath) && !hasValidCache(); }

void ImageBlock::clearSessionRenderFailures() { failedImageCount = 0; }

#ifdef CROSSPOINT_PSRAM_IMAGE_CACHE
// The PSRAM cache is deliberately not tied to a single page render: its whole
// point is that a back-turn or a re-visit still hits RAM. Slots are reclaimed
// by LRU eviction, so the reader's per-page release becomes a no-op.
void ImageBlock::releaseRenderCache() {}

void ImageBlock::clearPxcLru() {
  size_t freed = 0;
  for (auto& slot : pxcLru) {
    if (!slot.image) continue;
    freed += slot.capacity;
    pxcLruDrop(slot);
  }
  if (freed) LOG_DBG("IMG", "Released %u bytes of PSRAM image cache", static_cast<unsigned>(freed));
}

bool adoptPxcImage(const std::string& cachePath, uint8_t* pxcImage, const size_t byteCount) {
  if (!pxcImage || byteCount <= PXC_HEADER_BYTES) return false;
  uint16_t width = 0;
  uint16_t height = 0;
  memcpy(&width, pxcImage, 2);
  memcpy(&height, pxcImage + 2, 2);
  if (width == 0 || height == 0) return false;
  // The donated block must be exactly one .pxc image, or a render would walk
  // off the end of it.
  const size_t payload = static_cast<size_t>((width + 3) / 4) * height;
  if (payload > PXC_MAX_PAYLOAD_BYTES || PXC_HEADER_BYTES + payload != byteCount) return false;

  const uint64_t hash = imagePathHash(cachePath);
  PxcLruSlot& slot = pxcLruSlotFor(hash);
  pxcLruDrop(slot);  // takes ownership of a whole block; never reuses the old one
  slot.image = pxcImage;
  slot.capacity = byteCount;
  slot.hash = hash;
  slot.width = width;
  slot.height = height;
  slot.lastUse = ++pxcLruClock;
  LOG_DBG("IMG", "Adopted decoded payload: %s (%dx%d)", cachePath.c_str(), width, height);
  return true;
}
#else
void ImageBlock::releaseRenderCache() { releasePxcSlot(); }
#endif

void ImageBlock::renderPlaceholder(GfxRenderer& renderer, const int x, const int y) const {
  renderer.fillRect(x, y, width, height, true);
  if (width > 2 && height > 2) {
    renderer.fillRect(x + 1, y + 1, width - 2, height - 2, false);
  }
}

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  // The font-prewarm scan pass only accumulates glyphs; an image contributes
  // none, and its DirectPixelWriter output bypasses the renderer's scan-mode
  // suppression, so it would otherwise do a full (discarded) cache render every
  // page view. Skip it here. The image still draws in the real BW/grayscale
  // passes; on first view this just moves the one-time decode to the BW pass.
  FontCacheManager* fcm = renderer.getFontCacheManager();
  if (fcm && fcm->isScanning()) return;

  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check render position using logical screen dimensions
  if (x < 0 || y < 0 || x + width > screenWidth || y + height > screenHeight) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return;
  }

  // Tiled grayscale (#2190): skip the whole image when it doesn't touch the
  // active band. The per-pixel writer already clips off-band pixels, but without
  // this each of the ~7 bands per plane re-ran the full cache load / pixel walk
  // and discarded the result — the dominant cost of AA on image pages. The check
  // is orientation-aware and returns true when no strip is active, so the BW
  // pass and non-tiled controllers render the image exactly as before.
  if (!renderer.glyphIntersectsStrip(x, y, x + width - 1, y + height - 1)) {
    return;
  }

  if (imageFailedThisSession(imagePath)) {
    renderPlaceholder(renderer, x, y);
    return;
  }

  // Try to render from cache first
  std::string cachePath = getCachePath(imagePath);
  if (renderFromCache(renderer, cachePath, x, y, width, height)) {
    renderer.preserveImagePolarity(x, y, width, height);
    return;  // Successfully rendered from cache
  }

  // The build only header-probed the image for dimensions; pull the actual
  // file out of the book now, on first visit to the page.
  if (!srcPath.empty() && extractFn && !Storage.exists(imagePath.c_str())) {
    LOG_DBG("IMG", "Lazy-extracting %s -> %s", srcPath.c_str(), imagePath.c_str());
    if (!extractFn(extractCtx, srcPath.c_str(), imagePath.c_str())) {
      LOG_ERR("IMG", "Lazy extraction failed: %s", srcPath.c_str());
    }
  }

  // No cache - need to decode the image
  // Check if image file exists
  HalFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  config.cachePath = cachePath;      // Enable caching during decode

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  renderer.preserveImagePolarity(x, y, width, height);
  LOG_DBG("IMG", "Decode successful");
}

bool ImageBlock::serialize(HalFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writeString(file, srcPath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile& file) {
  std::string path;
  std::string src;
  serialization::readString(file, path);
  serialization::readString(file, src);
  int16_t w, h;
  serialization::readPod(file, w);
  serialization::readPod(file, h);
  return std::unique_ptr<ImageBlock>(new (std::nothrow) ImageBlock(path, src, w, h));
}

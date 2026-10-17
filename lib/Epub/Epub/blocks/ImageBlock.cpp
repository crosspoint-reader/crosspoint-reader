#include "ImageBlock.h"

#include <BuildArena.h>  // image_scratch::canServe needs the complete type
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Serialization.h>

#include "../../../../src/fontIds.h"
#include "../../Epub.h"
#include "../converters/DirectPixelWriter.h"
#include "../converters/ImageDecoderFactory.h"
#include "../converters/PixelCache.h"
#include "../converters/PngToFramebufferConverter.h"

// Cache file format (see PixelCache::PXC_MAGIC):
// - uint16_t magic/version (high bit always set, distinguishing it from the legacy
//   header that began with the width; legacy files are deleted on read)
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

namespace {
// Cut to MAX_ALT_TEXT_BYTES, never inside a UTF-8 sequence: a character the cut would split goes
// whole.
void capAltText(std::string& alt) {
  if (alt.size() <= ImageBlock::MAX_ALT_TEXT_BYTES) return;
  size_t cut = ImageBlock::MAX_ALT_TEXT_BYTES;
  while (cut > 0 && (static_cast<uint8_t>(alt[cut]) & 0xC0) == 0x80) cut--;
  alt.resize(cut);
}

// serialization::readString, except that it never holds more than the capped alt text: a cache
// written before the cap may carry up to MAX_STRING_LENGTH. One byte past the cap is read so the
// cut can see whether it falls inside a character; the rest is skipped.
bool readAltText(FsFile& file, std::string& alt) {
  uint32_t len = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&len), sizeof(len)) != sizeof(len)) return false;
  if (len > serialization::MAX_STRING_LENGTH) {
    file.seekCur(static_cast<int64_t>(len));
    return false;
  }
  const uint32_t keep = std::min<uint32_t>(len, ImageBlock::MAX_ALT_TEXT_BYTES + 1);
  alt.resize(keep);
  if (keep > 0 && file.read(reinterpret_cast<uint8_t*>(&alt[0]), keep) != static_cast<int>(keep)) return false;
  if (len > keep && !file.seekCur(static_cast<int64_t>(len - keep))) return false;
  capAltText(alt);
  return true;
}
}  // namespace

ImageBlock::ImageBlock(std::string imagePath, int16_t width, int16_t height, std::string altText)
    : imagePath(std::move(imagePath)), altText(std::move(altText)), width(width), height(height) {
  capAltText(this->altText);
}

ImageBlock::ImageBlock(std::string imagePath, int16_t width, int16_t height, std::string altText,
                       std::string epubFilePath, std::string epubEntryPath)
    : imagePath(std::move(imagePath)),
      altText(std::move(altText)),
      width(width),
      height(height),
      epubFilePath_(std::move(epubFilePath)),
      epubEntryPath_(std::move(epubEntryPath)) {
  capAltText(this->altText);
}

bool ImageBlock::ensureExtracted() const {
  if (Storage.exists(imagePath.c_str())) return true;
  if (epubFilePath_.empty() || epubEntryPath_.empty()) {
    LOG_ERR("IMG", "Image missing and no EPUB source: %s", imagePath.c_str());
    return false;
  }
  LOG_TRC("IMG", "Lazy-extracting image: %s -> %s", epubEntryPath_.c_str(), imagePath.c_str());
  Epub epub(epubFilePath_, "/.crosspoint");
  // Extraction runs inside the reader's warm pass, which has already borrowed the secondary
  // framebuffer as image_scratch for the decoders — but the ZIP inflate ring was the one 32 KB
  // contiguous block in the image path still taken from the heap, and it is the first to fail
  // when the heap is fragmented. Device-measured on X4 at contig=13300: every image on the page
  // logged "Failed to init inflate reader" and rendered as nothing at all. The extract finishes
  // and gives the block back before the decode starts, so the two never overlap in the arena.
  BuildArena* const arena = image_scratch::canServe(Epub::EXTRACT_ARENA_BYTES) ? image_scratch::get() : nullptr;
  if (!epub.extractItemToFile(epubEntryPath_, imagePath, arena)) {
    LOG_ERR("IMG", "Lazy extraction failed: %s", epubEntryPath_.c_str());
    return false;
  }
  LOG_TRC("IMG", "Lazy extraction done: %s", imagePath.c_str());
  return true;
}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace image_scratch {
namespace {
BuildArena* g_arena = nullptr;
}
BuildArena* get() { return g_arena; }
void set(BuildArena* arena) { g_arena = arena; }
bool canServe(const size_t bytes) {
  if (!g_arena || !g_arena->valid()) return false;
  const size_t used = g_arena->used();
  const size_t capacity = g_arena->capacity();
  if (used >= capacity) return false;
  // alloc() pads the cursor up to the requested alignment before the block, so budget for the
  // worst case rather than reporting room the allocator would then refuse.
  const size_t remaining = capacity - used;
  constexpr size_t ALIGN_SLACK = alignof(std::max_align_t);
  return remaining >= bytes && remaining - bytes >= ALIGN_SLACK;
}
}  // namespace image_scratch

namespace {

std::string withSuffix(const std::string& imagePath, const std::string& suffix) {
  size_t dot = imagePath.rfind('.');
  if (dot != std::string::npos) return imagePath.substr(0, dot) + suffix;
  return imagePath + suffix;
}

// BW-plane cache: 1-bit Atkinson, only values 0/3, for AA-off rendering.
// Deliberately NOT keyed by the tone filter: tone mapping is applied only to the
// grayscale variant (see ImageBlock::render), so these pixels are identical whatever
// the filter is set to. Keying them too would fork every BW cache on SD for nothing.
std::string getBwCachePath(const std::string& imagePath) { return withSuffix(imagePath, ".1bit.pxc"); }

// Grayscale cache: 4-level Bayer (0–3), replayed in GRAYSCALE_LSB/MSB passes when AA is on.
// Like the BW plane above, these pixels carry no tone correction, so one cache per image
// serves every display setting — the name needs no key and never forks.
std::string getGrayscaleCachePath(const std::string& imagePath) { return withSuffix(imagePath, ".bayer.pxc"); }

// Decode a PNG straight out of the EPUB, with no extraction to SD first.
//
// Only possible when the ZIP STORES the entry (already-compressed formats usually are): the
// bytes in the archive are then the file, and PngStreamDecoder reads forward and seeks only
// relatively, so a handle parked at the entry's first byte is all it needs.
//
// Worth doing because the extract it replaces is pure copying — 857 KB measured at ~255 KB/s,
// 3.36 s of the 14.93 s an uncached cover cost, plus 857 KB written to the card. Deflated
// entries cannot take this path (see Epub::getStoredItemRange) and still extract.
//
// Returns false if anything at all is not right, leaving the caller to extract and retry: a
// failed attempt costs one bounded seek and whatever partial decode happened, and PixelCache
// deletes its own partial file, so the fallback starts clean.
bool decodePngInPlace(const std::string& epubFilePath, const std::string& epubEntryPath, GfxRenderer& renderer,
                      const RenderConfig& config) {
  Epub epub(epubFilePath, "/.crosspoint");
  uint32_t offset = 0;
  uint32_t size = 0;
  if (!epub.getStoredItemRange(epubEntryPath, &offset, &size) || size == 0) {
    // Logged because it decides seconds: a deflated entry has to be extracted first, and
    // otherwise the shortcut's absence is invisible in a trace (the extract itself is TRC).
    LOG_DBG("IMG", "Not stored in the archive, extracting first: %s", epubEntryPath.c_str());
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("IMG", epubFilePath, file)) return false;
  if (!file.seekSet(offset)) {
    file.close();
    return false;
  }
  LOG_DBG("IMG", "Decoding in place from archive: %s (%u bytes at %u)", epubEntryPath.c_str(),
          static_cast<unsigned>(size), static_cast<unsigned>(offset));
  const bool ok = PngToFramebufferConverter::decodeOpenFile(file, epubEntryPath, renderer, config);
  file.close();
  return ok;
}

// srcYOffset: first source row to render (0 = top of image).
// srcHeight:  number of rows to render (0 = full image from srcYOffset).
bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight, int srcYOffset = 0, int srcHeight = 0) {
  FsFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  // Version check first: a .pxc written by an older firmware may carry pixel
  // content with known rendering bugs baked in (e.g. the MCU-order dither grid),
  // and would otherwise be replayed forever without re-decoding. Delete it so
  // the caller falls through to a fresh decode, which rewrites the cache.
  uint16_t magic;
  if (cacheFile.read(&magic, 2) != 2 || !PixelCache::magicIsValid(magic)) {
    cacheFile.close();
    LOG_INF("IMG", "Stale/unversioned pixel cache (0x%04X), deleting: %s", magic, cachePath.c_str());
    Storage.remove(cachePath.c_str());
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    cacheFile.close();
    return false;
  }

  // Verify width is close (allow 1 pixel tolerance for rounding differences).
  // Height tolerance is widened to allow cropped renders (srcHeight < cachedHeight).
  if (abs(cachedWidth - expectedWidth) > 1) {
    LOG_ERR("IMG", "Cache width mismatch: %d vs %d", cachedWidth, expectedWidth);
    cacheFile.close();
    return false;
  }

  // Resolve crop window against actual cached dimensions
  if (srcYOffset < 0) srcYOffset = 0;
  if (srcYOffset >= static_cast<int>(cachedHeight)) {
    cacheFile.close();
    return false;
  }
  // Cap rows by both the cache height and the caller's expected height to prevent
  // overrunning the framebuffer when a 1-pixel cache rounding difference occurs.
  const int maxRows = std::min(static_cast<int>(cachedHeight) - srcYOffset, expectedHeight - srcYOffset);
  const int rowsToRender = (srcHeight > 0) ? std::min(srcHeight, maxRows) : maxRows;

  LOG_TRC("IMG", "Loading from cache: %s (%dx%d) srcY=%d rows=%d", cachePath.c_str(), cachedWidth, cachedHeight,
          srcYOffset, rowsToRender);

  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte

  // Seek directly to the first row of interest — no need to iterate skipped rows.
  // Cache layout: header (magic + width + height) followed by rows in order.
  if (srcYOffset > 0) {
    const uint32_t seekPos = static_cast<uint32_t>(PixelCache::PXC_HEADER_BYTES) +
                             static_cast<uint32_t>(srcYOffset) * static_cast<uint32_t>(bytesPerRow);
    if (!cacheFile.seekSet(seekPos)) {
      LOG_ERR("IMG", "Cache seek failed to row %d", srcYOffset);
      cacheFile.close();
      return false;
    }
  }

  // Read several rows per SD access. A full-page image is replayed from cache up to
  // 3x per page with AA on (BW plane + both grayscale planes), and a one-row-per-read
  // loop here means hundreds of tiny reads through the storage mutex + SdFat each time —
  // the dominant cost of displaying an image page. Batching rows into a ~4KB buffer
  // cuts that down dramatically without holding the whole image.
  // (Ported from upstream commit d9bcef7a, crosspoint-reader#2230, when the multi-strip
  // grayscale passes made this ~14x; the strips are gone but the batching still pays.)
  int rowsPerRead = 4096 / bytesPerRow;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > rowsToRender) rowsPerRead = rowsToRender;
  uint8_t* readBuffer = (uint8_t*)malloc((size_t)rowsPerRead * bytesPerRow);
  if (!readBuffer) {
    // Fall back to a single-row buffer under memory pressure.
    rowsPerRead = 1;
    readBuffer = (uint8_t*)malloc(bytesPerRow);
  }
  if (!readBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    cacheFile.close();
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = 0; row < rowsToRender; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (rowsToRender - row < rowsPerRead) ? (rowsToRender - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRow;
      const int bytesRead = cacheFile.read(readBuffer, bytes);
      if (bytesRead < 0 || static_cast<size_t>(bytesRead) != bytes) {
        LOG_ERR("IMG", "Cache read error at row %d", srcYOffset + row);
        free(readBuffer);
        cacheFile.close();
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* rowBuffer = readBuffer + (size_t)bufferRow * bytesPerRow;
    bufferRow++;

    const int destY = y + row;
    pw.beginRow(destY);
    // Column window for the active write target. The strip-based grayscale passes are
    // gone (isStripActive() is hardcoded false), so this is the full image width today
    // and the range doubles as a bounds guard.
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
  cacheFile.close();
  LOG_TRC("IMG", "Cache render complete");
  return true;
}

}  // namespace

bool ImageBlock::isLargeImage() const {
  if (largeImageCached_ >= 0) {
    return largeImageCached_ != 0;
  }
  size_t sourceBytes = 0;
  // Cheapest source first: once the image has been extracted (a re-render, or the other cache
  // variant decoded earlier in the same pass) the answer is a stat away.
  FsFile file;
  if (Storage.openFileForRead("IMG", imagePath, file)) {
    sourceBytes = file.size();
    file.close();
  } else if (!epubFilePath_.empty() && !epubEntryPath_.empty()) {
    // Not extracted yet: ask the archive. One central-directory scan, and only ever on a
    // pixel-cache miss — the very next thing that happens is either a placeholder (cheap) or a
    // decode that costs seconds, so the scan is noise against both.
    Epub epub(epubFilePath_, "/.crosspoint");
    if (!epub.getItemSize(epubEntryPath_, &sourceBytes)) {
      sourceBytes = 0;  // unknown: treat as not-large, i.e. render it rather than hide it
    }
  }
  largeImageCached_ = static_cast<int8_t>(sourceBytes > LARGE_IMAGE_SOURCE_BYTES ? 1 : 0);
  if (largeImageCached_ != 0) {
    LOG_DBG("IMG", "Large image (%u bytes): %s", static_cast<uint32_t>(sourceBytes), imagePath.c_str());
  }
  return largeImageCached_ != 0;
}

bool ImageBlock::hasPixelCache() const { return Storage.exists(getBwCachePath(imagePath).c_str()); }

bool ImageBlock::hasGrayscaleCache() const { return Storage.exists(getGrayscaleCachePath(imagePath).c_str()); }

bool ImageBlock::dropCoarseCache(const bool monochromeOutput) const {
  const std::string& cachePath = monochromeOutput ? getBwCachePath(imagePath) : getGrayscaleCachePath(imagePath);
  FsFile cacheFile;
  if (!Storage.exists(cachePath.c_str()) || !Storage.openFileForRead("IMG", cachePath, cacheFile)) return false;
  uint16_t magic = 0;
  const bool coarse = cacheFile.read(&magic, 2) == 2 && magic == PixelCache::PXC_MAGIC_COARSE;
  cacheFile.close();
  if (!coarse) return false;
  LOG_INF("IMG", "Dropping coarse pixel cache for a full decode: %s", cachePath.c_str());
  Storage.remove(cachePath.c_str());
  return true;
}

bool ImageBlock::wouldShowPlaceholder(bool forceLoad, bool monochromeOutput) const {
  if (forceLoad) return false;
  // The cache before the size: a cached image is never a placeholder, and a stat settles that.
  // isLargeImage() opens the extracted image, and when there is none (an image decoded straight
  // out of the archive) logs the failed open and scans the EPUB's central directory -- ~30 ms on
  // an X4. Its memo lives on the block and pages are deserialized afresh for every render, so
  // asking it first paid that on every render of a cached image, for an answer that could not
  // change the result.
  // Check only the cache variant that render() will actually use for this mode.
  const std::string& cachePath = monochromeOutput ? getBwCachePath(imagePath) : getGrayscaleCachePath(imagePath);
  if (Storage.exists(cachePath.c_str())) return false;
  return isLargeImage();
}

void ImageBlock::renderGrayscaleFromCache(GfxRenderer& renderer, const int x, const int y) const {
  renderFromCache(renderer, getGrayscaleCachePath(imagePath), x, y, width, height, srcYOffset_, srcHeight_);
}

std::unique_ptr<ImageBlock> ImageBlock::makeCrop(const int16_t srcYOffset, const int16_t srcHeight) const {
  auto crop =
      std::unique_ptr<ImageBlock>(new ImageBlock(imagePath, width, height, altText, epubFilePath_, epubEntryPath_));
  crop->srcYOffset_ = srcYOffset;
  crop->srcHeight_ = srcHeight;
  return crop;
}

bool ImageBlock::placeholderOnly_ = false;
uint16_t ImageBlock::placeholdersDrawn_ = 0;

void ImageBlock::renderPlaceholder(GfxRenderer& renderer, const int x, const int y, const bool loading) const {
  constexpr int BORDER = 1;
  constexpr int PADDING = 6;

  renderer.drawRect(x, y, width, height, BORDER, true);

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const bool hasAlt = !altText.empty();
  // Loading: the alt text (if any) and one "indexing" line. Large image: three lines as before.
  const int lineCount = loading ? (hasAlt ? 2 : 1) : (hasAlt ? 3 : 2);
  const int totalTextH = lineH * lineCount;

  if (lineH > 0 && width > PADDING * 2 && height > totalTextH + PADDING * 2) {
    const int textX = x + PADDING;
    const int textY = y + (height - totalTextH) / 2;
    if (loading) {
      if (hasAlt) renderer.drawText(UI_10_FONT_ID, textX, textY, altText.c_str());
      renderer.drawText(UI_10_FONT_ID, textX, textY + lineH * (lineCount - 1), tr(STR_INDEXING));
      return;
    }
    renderer.drawText(UI_10_FONT_ID, textX, textY, tr(STR_LARGE_IMAGE));
    if (hasAlt) {
      renderer.drawText(UI_10_FONT_ID, textX, textY + lineH, altText.c_str());
    }
    renderer.drawText(UI_10_FONT_ID, textX, textY + lineH * (lineCount - 1), tr(STR_PRESS_CONFIRM_TO_LOAD));
  }
}

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y, const bool forceLoad,
                        const bool monochromeOutput, const bool alsoCacheOtherVariant) {
  // The font-prewarm scan pass only accumulates glyphs; an image contributes
  // none, and its DirectPixelWriter output bypasses the renderer's scan-mode
  // suppression, so it would otherwise do a full (discarded) cache render every
  // page view. Skip it here. The image still draws in the real BW/grayscale
  // passes; on first view this just moves the one-time decode to the BW pass.
  // (Ported from upstream commit d9bcef7a, crosspoint-reader#2230.)
  if (renderer.isFontCacheScanning()) return;

  const int renderedHeight = srcHeight_ > 0 ? srcHeight_ : height;
  LOG_TRC("IMG", "Rendering image at %d,%d: %s (%dx%d) srcY=%d rendH=%d mono=%d", x, y, imagePath.c_str(), width,
          height, srcYOffset_, renderedHeight, monochromeOutput ? 1 : 0);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check against the rendered (cropped) height, not the full image height.
  if (x < 0 || y < 0 || x + width > screenWidth || y + renderedHeight > screenHeight) {
    LOG_ERR("IMG", "Render bounds rejected: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, renderedHeight,
            screenWidth, screenHeight);
    return;
  }

  // Select cache path based on rendering mode
  const std::string cachePath = monochromeOutput ? getBwCachePath(imagePath) : getGrayscaleCachePath(imagePath);

  // Try to render from pixel cache first (always, regardless of forceLoad)
  if (renderFromCache(renderer, cachePath, x, y, width, renderedHeight, srcYOffset_, srcHeight_)) {
    return;
  }

  // No pixel cache — check if this is a large image that should show a placeholder
  // A mid-build draw (PlaceholderOnlyScope): the cache above was the only cheap source; no
  // decode on the build's heap.
  if (placeholderOnly_) {
    if (placeholdersDrawn_ < UINT16_MAX) placeholdersDrawn_++;
    renderPlaceholder(renderer, x, y, /*loading=*/true);
    return;
  }

  if (wouldShowPlaceholder(forceLoad, monochromeOutput)) {
    LOG_DBG("IMG", "Large image placeholder at %d,%d (%dx%d): %s", x, y, width, height, imagePath.c_str());
    renderPlaceholder(renderer, x, y);
    return;
  }

  // Build the decode config before deciding HOW to reach the bytes: the in-place shortcut
  // below needs the same config the extracted path would use.
  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.monochromeOutput = monochromeOutput;
  config.performanceMode = false;
  config.useExactDimensions = true;
  config.cachePath = cachePath;
  // One inflate, both caches. Only set on a real decode, which is the only place it can pay:
  // the cache-hit and placeholder paths above already returned.
  if (alsoCacheOtherVariant) {
    config.companionCachePath = monochromeOutput ? getGrayscaleCachePath(imagePath) : getBwCachePath(imagePath);
  }
  // Deliberately no adaptive tone on either variant: both .pxc files are dithered straight
  // from the raw luminance. The curve has to be derived from a completed histogram, and a
  // PNG cannot be rewound to build one -- it costs a second full inflate of every image, on
  // the warm pass that already gates how fast a page with pictures opens. The correction is
  // still offered on the sleep screen, which pays it once per wake rather than once per page
  // and keys its single cache by the filter id.
  //
  // Leaving both variants untoned is also what keeps them interchangeable inputs: they now
  // differ only in ditherer (1-bit Atkinson vs 4-level Bayer) over an identical grey stream.

  // Shortcut: a PNG the archive stores uncompressed needs no extraction at all — decode it
  // where it lies (see decodePngInPlace). Only attempted while the file is genuinely absent
  // from SD; once extracted, reading the plain file is simpler and no slower.
  //
  // The extension is a hint, not a guarantee (a .png that is really an AVIF is a thing that
  // happens), but it costs nothing to be wrong: the decoder rejects the signature and we fall
  // through to the extract exactly as before.
  if (!epubFilePath_.empty() && !epubEntryPath_.empty() && !Storage.exists(imagePath.c_str()) &&
      FsHelpers::hasPngExtension(imagePath) && decodePngInPlace(epubFilePath_, epubEntryPath_, renderer, config)) {
    return;
  }

  // Ensure the image is extracted to SD (lazy extraction if not already present).
  if (!ensureExtracted()) {
    LOG_ERR("IMG", "Image unavailable: %s", imagePath.c_str());
    return;
  }

  FsFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found after extraction: %s", imagePath.c_str());
    return;
  }
  size_t fileSize = file.size();
  // The bytes pick the decoder, not the name: FB2 conversions reference images with no extension
  // at all (cached as .img), and some books call a PNG .jpg. A truncated extract replaced below
  // starts with the same bytes, so the answer holds for it too.
  const ImageFormatDetector::Format format = ImageFormatDetector::detect(file);
  file.close();
  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    return;
  }
  // An extract on the card is trusted as-is, so one cut short (a reset mid-write, before the
  // extract became atomic) was decoded as garbage on every visit of its page -- "no SOF marker"
  // for a JPEG whose header sits 14 KB in (X3 2026-09-25). One central-directory lookup per
  // first decode catches it: a size that differs from the entry's is re-extracted, once.
  if (!epubFilePath_.empty() && !epubEntryPath_.empty()) {
    Epub epub(epubFilePath_, "/.crosspoint");
    size_t entrySize = 0;
    if (epub.getItemSize(epubEntryPath_, &entrySize) && entrySize != 0 && entrySize != fileSize) {
      LOG_ERR("IMG", "Extract is %u bytes, entry is %u: re-extracting %s", static_cast<unsigned>(fileSize),
              static_cast<unsigned>(entrySize), epubEntryPath_.c_str());
      Storage.remove(imagePath.c_str());
      if (!ensureExtracted()) return;
      fileSize = entrySize;
    }
  }

  LOG_TRC("IMG", "Decoding and caching: %s", imagePath.c_str());

  // Adapted from crosspoint-reader PR #2386 (artwen, @artwem), which sniffs in the factory when the
  // extension is unknown; here the content always decides, so a wrong extension decodes too.
  ImageToFramebufferDecoder* decoder = format != ImageFormatDetector::Format::Unknown
                                           ? ImageDecoderFactory::getDecoder(format)
                                           : ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    return;
  }

  LOG_TRC("IMG", "Using %s decoder", decoder->getFormatName());

  // A JPEG decode stopped for input parks here and the next decode of it resumes (see
  // RenderConfig::checkpointPath): the reader's image lane gives its region back on every page
  // turn, and used to throw the decode away with it.
  const bool jpeg = format == ImageFormatDetector::Format::Jpeg;
  if (jpeg) config.checkpointPath = imagePath + ".ckpt";

  // A resumed decode draws only the rows after its park; the finished cache has them all.
  const bool resuming = jpeg && Storage.exists(config.checkpointPath.c_str());
  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    return;
  }
  if (resuming && !Storage.exists(config.checkpointPath.c_str())) {
    renderFromCache(renderer, cachePath, x, y, width, renderedHeight, srcYOffset_, srcHeight_);
  }
}

bool ImageBlock::serialize(FsFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  serialization::writeString(file, altText);
  serialization::writeString(file, epubFilePath_);
  serialization::writeString(file, epubEntryPath_);
  serialization::writePod(file, srcYOffset_);
  serialization::writePod(file, srcHeight_);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(FsFile& file) {
  std::string path;
  serialization::readString(file, path);
  int16_t w, h;
  serialization::readPod(file, w);
  serialization::readPod(file, h);
  std::string alt, epubFile, epubEntry;
  readAltText(file, alt);
  serialization::readString(file, epubFile);
  serialization::readString(file, epubEntry);
  int16_t srcYOffset = 0, srcHeight = 0;
  serialization::readPod(file, srcYOffset);
  serialization::readPod(file, srcHeight);
  auto block = std::unique_ptr<ImageBlock>(
      new ImageBlock(std::move(path), w, h, std::move(alt), std::move(epubFile), std::move(epubEntry)));
  block->srcYOffset_ = srcYOffset;
  block->srcHeight_ = srcHeight;
  return block;
}

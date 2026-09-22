#include "HomeCoverCache.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>

#include "UITheme.h"

namespace fui = freeink::ui;

void HomeCoverCache::begin() {
  // Cover regions do not overlap. Each may widen by one physical byte per row.
  coverCacheCapacity = renderer.getRegionByteSize(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()) +
                       MAX_COVERS * std::max(renderer.getScreenWidth(), renderer.getScreenHeight());
  coverCache = HalMemory::allocatePsram(coverCacheCapacity);
  if (!coverCache) {
    LOG_ERR("HOME", "PSRAM cover cache unavailable (%u bytes); rendering uncached", unsigned(coverCacheCapacity));
    coverCacheCapacity = 0;
  }
}

void HomeCoverCache::invalidate() {
  coverCacheUsed = 0;
  cachedCovers.fill(CachedCover{});
}

void HomeCoverCache::invalidate(size_t index) {
  if (index < cachedCovers.size()) cachedCovers[index].valid = false;
}

void HomeCoverCache::prepare() {
  const int orientation = static_cast<int>(renderer.getOrientation());
  if (coverCacheOrientation != orientation) {
    invalidate();
    coverCacheOrientation = orientation;
  }
}

void HomeCoverCache::readSize(const std::string& path, int& width, int& height) {
  width = height = 0;
  if (!path.empty() && Storage.exists(path.c_str()) && Storage.openFileForRead("HOME", path, coverFile)) {
    if (coverBitmap.parseHeaders() == BmpReaderError::Ok) {
      width = coverBitmap.getWidth();
      height = coverBitmap.getHeight();
    }
    coverFile.close();
  }
}

bool HomeCoverCache::paint(fui::Rect rect, size_t index, const std::string& path) {
  if (index >= cachedCovers.size()) return false;
  auto& cached = cachedCovers[index];
  if (coverCache && cached.valid && cached.rect.x == rect.x && cached.rect.y == rect.y &&
      cached.rect.width == rect.width && cached.rect.height == rect.height &&
      renderer.copyBufferToRegion(rect.x, rect.y, rect.width, rect.height, coverCache.get() + cached.offset,
                                  cached.bytes)) {
    return true;
  }
  cached.valid = false;
  bool drawn = false;
  if (!path.empty() && Storage.openFileForRead("HOME", path, coverFile)) {
    if (coverBitmap.parseHeaders() == BmpReaderError::Ok && coverBitmap.getWidth() > 0 && coverBitmap.getHeight() > 0) {
      drawn = GUI.drawCoverThumbFill(renderer, coverBitmap, Rect{rect.x, rect.y, rect.width, rect.height});
    }
    coverFile.close();
  }
  if (!drawn) GUI.drawCoverPlaceholder(renderer, Rect{rect.x, rect.y, rect.width, rect.height});
  if (coverCache) {
    const size_t needed = renderer.getRegionByteSize(rect.x, rect.y, rect.width, rect.height);
    if (needed > cached.bytes && needed <= coverCacheCapacity - coverCacheUsed) {
      cached.offset = coverCacheUsed;
      cached.bytes = needed;
      coverCacheUsed += needed;
    }
    if (needed > 0 && needed <= cached.bytes) {
      cached.rect = rect;
      cached.valid = renderer.copyRegionToBuffer(rect.x, rect.y, rect.width, rect.height,
                                                 coverCache.get() + cached.offset, cached.bytes);
    }
  }
  return true;  // The cover slot is painted, including fallback art.
}

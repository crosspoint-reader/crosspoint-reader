#pragma once

#include <Bitmap.h>
#include <FreeInkUICore.h>
#include <HalMemory.h>
#include <HalStorage.h>

#include <array>
#include <string>

class GfxRenderer;

// Screen-lifetime thumbnail decoding and PSRAM region snapshots.
class HomeCoverCache {
 public:
  static constexpr size_t MAX_COVERS = 7;
  explicit HomeCoverCache(GfxRenderer& renderer) : renderer(renderer) {}
  void begin();
  void prepare();
  void invalidate();
  void invalidate(size_t index);
  void readSize(const std::string& path, int& width, int& height);
  bool paint(freeink::ui::Rect rect, size_t index, const std::string& path);

 private:
  struct CachedCover {
    freeink::ui::Rect rect{};
    size_t offset = 0;
    size_t bytes = 0;
    bool valid = false;
  };
  HalMemory::PsramBuffer coverCache;
  std::array<CachedCover, MAX_COVERS> cachedCovers{};
  size_t coverCacheCapacity = 0;
  size_t coverCacheUsed = 0;
  int coverCacheOrientation = -1;

  HalFile coverFile;
  Bitmap coverBitmap{coverFile};
  GfxRenderer& renderer;
};

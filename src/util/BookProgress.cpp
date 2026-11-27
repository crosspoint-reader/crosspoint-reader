#include "BookProgress.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>

namespace {
uint32_t readLe32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
}  // namespace

int loadBookProgress(const std::string& path) {
  uint8_t data[10]{};
  if (FsHelpers::hasReflowableBookExtension(path)) {
    const auto cache = Epub::cachePathFor(path, "/.crosspoint");
    HalFile file;
    if (!Storage.exists(path.c_str()) || !Storage.openFileForRead("HOME", cache + "/progress.bin", file)) return -1;
    const auto size = file.size();
    if ((size != 4 && size != 6 && size != 10) || file.read(data, size) != static_cast<int>(size)) return -1;
    file.close();
    const uint16_t spine = data[0] | (data[1] << 8);
    const int page = data[2] | (data[3] << 8);
    const int total = size >= 6 ? data[4] | (data[5] << 8) : 0;
    uint32_t before, through, bookSize, first;
    if (!BookMetadataCache::readProgressSizes(cache, spine, before, through, bookSize, first)) return -1;
    if (Txt::isTxtOrMd(path) && !Txt::validateCache(path, cache, first)) return -1;
    const float fraction = total > 0 && page != UINT16_MAX ? std::clamp(float(page) / total, 0.0f, 1.0f) : 0;
    const float progress = (float(before) + fraction * float(through - before)) / float(bookSize);
    return std::clamp(static_cast<int>(progress * 100 + 0.5f), 0, 100);
  }
  if (FsHelpers::hasXtcExtension(path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("HOME", "OOM: XTC progress metadata");
      return -1;
    }
    HalFile file;
    if (!Storage.openFileForRead("HOME", xtc->getCachePath() + "/progress.bin", file) || file.read(data, 4) != 4)
      return -1;
    file.close();
    if (!xtc->load()) return -1;
    const uint32_t page = readLe32(data);
    if (xtc->getPageCount() == 0) return -1;
    if (page >= xtc->getPageCount()) return 100;
    return xtc->calculateProgress(page);
  }
  return -1;
}

#pragma once

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include <optional>

namespace EpubReaderUtils {

struct ReturnPoint {
  int spineIndex;
  int pageNumber;
  int pageCount;
};

// Filename for the persisted return point inside an EPUB's cache directory.
// Six bytes: spine, page, pageCount (little-endian uint16 each).
inline constexpr const char RETURN_POINT_FILENAME[] = "/returnPoint.bin";

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(Epub& epub, int spineIndex, int pageNumber, int pageCount) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  HalFile f;
  if (!Storage.openFileForWrite("ERS", epub.getCachePath() + "/progress.bin", f)) {
    LOG_ERR("ERS", "Could not open progress file for write!");
    return false;
  }
  uint8_t data[6];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  const size_t written = f.write(data, sizeof(data));
  if (written != sizeof(data)) {
    LOG_ERR("ERS", "Short write saving progress: %u/%u bytes", (unsigned)written, (unsigned)sizeof(data));
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%d page=%d", spineIndex, pageNumber);
  return true;
}

inline bool saveReturnPoint(Epub& epub, const ReturnPoint& point) {
  if (point.spineIndex < 0 || point.spineIndex > 0xFFFF || point.pageNumber < 0 || point.pageNumber > 0xFFFF ||
      point.pageCount < 0 || point.pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Return point out of range: spine=%d page=%d count=%d", point.spineIndex, point.pageNumber,
            point.pageCount);
    return false;
  }
  HalFile f;
  if (!Storage.openFileForWrite("ERS", epub.getCachePath() + RETURN_POINT_FILENAME, f)) {
    LOG_ERR("ERS", "Could not open return point file for write!");
    return false;
  }
  uint8_t data[6];
  data[0] = point.spineIndex & 0xFF;
  data[1] = (point.spineIndex >> 8) & 0xFF;
  data[2] = point.pageNumber & 0xFF;
  data[3] = (point.pageNumber >> 8) & 0xFF;
  data[4] = point.pageCount & 0xFF;
  data[5] = (point.pageCount >> 8) & 0xFF;
  const size_t written = f.write(data, sizeof(data));
  if (written != sizeof(data)) {
    LOG_ERR("ERS", "Short write saving return point: %u/%u bytes", (unsigned)written, (unsigned)sizeof(data));
    return false;
  }
  LOG_DBG("ERS", "Return point saved: spine=%d page=%d", point.spineIndex, point.pageNumber);
  return true;
}

inline std::optional<ReturnPoint> loadReturnPoint(const Epub& epub) {
  const std::string path = epub.getCachePath() + RETURN_POINT_FILENAME;
  HalFile f;
  if (!Storage.openFileForRead("ERS", path, f)) {
    return std::nullopt;
  }
  uint8_t data[6];
  const int bytesRead = f.read(data, sizeof(data));
  if (bytesRead != static_cast<int>(sizeof(data))) {
    const bool removed = Storage.remove(path.c_str());
    LOG_DBG("ERS", "Return point file short read: %d/%u bytes; removed=%d", bytesRead, (unsigned)sizeof(data), removed);
    return std::nullopt;
  }
  ReturnPoint p;
  p.spineIndex = data[0] | (data[1] << 8);
  p.pageNumber = data[2] | (data[3] << 8);
  p.pageCount = data[4] | (data[5] << 8);
  const int spineCount = epub.getSpineItemsCount();
  if (p.spineIndex < 0 || p.spineIndex >= spineCount) {
    LOG_DBG("ERS", "Return point spine %d out of range [0,%d); discarding", p.spineIndex, spineCount);
    const bool removed = Storage.remove(path.c_str());
    LOG_DBG("ERS", "Stale return point removed=%d", removed);
    return std::nullopt;
  }
  LOG_DBG("ERS", "Return point loaded: spine=%d page=%d", p.spineIndex, p.pageNumber);
  return p;
}

inline void clearReturnPoint(const Epub& epub) {
  const std::string path = epub.getCachePath() + RETURN_POINT_FILENAME;
  if (Storage.remove(path.c_str())) {
    LOG_DBG("ERS", "Return point cleared");
  }
}

}  // namespace EpubReaderUtils

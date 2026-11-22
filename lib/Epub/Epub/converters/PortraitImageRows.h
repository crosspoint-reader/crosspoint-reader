#pragma once

#include <ImageOpsConfig.h>

#if CROSSPOINT_ACCELERATED_IMAGES
#include <ImageOps.h>
#include <Memory.h>

#include "DirectPixelWriter.h"

namespace portraitImage {

// One optional, decode-owned buffer for up to eight 16-byte-aligned rows.
class LevelRows {
 public:
  LevelRows() = default;
  LevelRows(const LevelRows&) = delete;
  LevelRows& operator=(const LevelRows&) = delete;

  // Called once, after decoder and cache setup.
  void allocate(int width, const char* logTag);

  uint8_t* data() const { return aligned; }
  uint8_t* row(int index) const { return aligned + index * rowStride; }
  size_t stride() const { return rowStride; }
  int width() const { return visibleWidth; }

 private:
  std::unique_ptr<uint8_t[]> storage;
  uint8_t* aligned{nullptr};
  size_t rowStride{0};
  int visibleWidth{0};
};

// The caller has already positioned the optional cache writer at this row.
// Copy out the levels now; the cache band may be cleared at the next advanceTo().
void finishDitheredRow(uint8_t* row, int width, int x, int y, DirectCacheWriter* cache);

}  // namespace portraitImage
#endif

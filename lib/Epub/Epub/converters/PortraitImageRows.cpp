#include "PortraitImageRows.h"

#if CROSSPOINT_ACCELERATED_IMAGES
#include <Logging.h>

#include <limits>

namespace portraitImage {
void LevelRows::allocate(int width, [[maybe_unused]] const char* logTag) {
  if (width <= 0) return;
  constexpr size_t ALIGNMENT_SLACK = 15;
  constexpr size_t MAX_ROWS = 8;
  constexpr size_t MAX_SIZE = std::numeric_limits<size_t>::max();
  const size_t rowWidth = static_cast<size_t>(width);
  if (rowWidth > MAX_SIZE - ALIGNMENT_SLACK) return;
  const size_t stride = (rowWidth + ALIGNMENT_SLACK) & ~ALIGNMENT_SLACK;
  if (stride > (MAX_SIZE - ALIGNMENT_SLACK) / MAX_ROWS) return;
  const size_t bytes = stride * MAX_ROWS + ALIGNMENT_SLACK;
  storage = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!storage) {
    LOG_ERR(logTag, "Failed to allocate portrait levels (%u bytes), using scalar output", static_cast<unsigned>(bytes));
    return;
  }
  aligned = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(storage.get()) + ALIGNMENT_SLACK) &
                                       ~uintptr_t{ALIGNMENT_SLACK});
  rowStride = stride;
  visibleWidth = width;
}

void finishDitheredRow(uint8_t* row, int width, int x, int y, DirectCacheWriter* cache) {
  imageops::ditherBayer4LevelSpan(row, row, width, x, y);
  if (cache) {
    for (int column = 0; column < width; ++column) cache->writePixel(x + column, row[column]);
  }
}
}  // namespace portraitImage
#endif

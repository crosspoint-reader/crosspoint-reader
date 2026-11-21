#include "OtTable.h"

namespace ot {

namespace {

inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

// Entries of `recordSize` bytes after a 4-byte header that fit in `t`: the
// declared count, clamped so the searches below can read without checks.
inline unsigned fittingCount(const Table& t, const unsigned recordSize) { return t.count16(2, 4, recordSize); }

}  // namespace

int coverageIndex(const Table& coverage, const uint32_t glyph) {
  const uint8_t* d = coverage.data();
  switch (coverage.u16(0)) {
    case 1: {
      // Sorted glyph array.
      const uint8_t* glyphs = d + 4;
      int lo = 0;
      int hi = static_cast<int>(fittingCount(coverage, 2)) - 1;
      while (lo <= hi) {
        const int mid = static_cast<int>((static_cast<unsigned>(lo) + static_cast<unsigned>(hi)) / 2);
        const uint16_t g = be16(glyphs + 2 * mid);
        if (glyph < g) {
          hi = mid - 1;
        } else if (glyph > g) {
          lo = mid + 1;
        } else {
          return mid;
        }
      }
      return NOT_COVERED;
    }
    case 2: {
      // Sorted ranges: start, end, startCoverageIndex.
      const uint8_t* ranges = d + 4;
      int lo = 0;
      int hi = static_cast<int>(fittingCount(coverage, 6)) - 1;
      while (lo <= hi) {
        const int mid = static_cast<int>((static_cast<unsigned>(lo) + static_cast<unsigned>(hi)) / 2);
        const uint8_t* r = ranges + 6 * mid;
        const uint16_t start = be16(r);
        if (glyph < start) {
          hi = mid - 1;
        } else if (glyph > be16(r + 2)) {
          lo = mid + 1;
        } else {
          return static_cast<int>(be16(r + 4) + (glyph - start));
        }
      }
      return NOT_COVERED;
    }
    default:
      return NOT_COVERED;
  }
}

uint16_t classOf(const Table& classDef, const uint32_t glyph) {
  switch (classDef.u16(0)) {
    case 1: {
      const uint16_t start = classDef.u16(2);
      const uint16_t count = classDef.u16(4);
      if (glyph < start || glyph - start >= count) return 0;
      return classDef.u16(6 + 2 * (glyph - start));
    }
    case 2: {
      const uint8_t* ranges = classDef.data() + 4;
      int lo = 0;
      int hi = static_cast<int>(fittingCount(classDef, 6)) - 1;
      while (lo <= hi) {
        const int mid = static_cast<int>((static_cast<unsigned>(lo) + static_cast<unsigned>(hi)) / 2);
        const uint8_t* r = ranges + 6 * mid;
        if (glyph < be16(r)) {
          hi = mid - 1;
        } else if (glyph > be16(r + 2)) {
          lo = mid + 1;
        } else {
          return be16(r + 4);
        }
      }
      return 0;
    }
    default:
      return 0;
  }
}

}  // namespace ot

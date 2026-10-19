#include "GlyphStoreFormat.h"

namespace glyphstore {

namespace {

// Index pages so that kIndexLoadPercent of the slots holds logPages of expectedRecord-byte records.
uint32_t indexPagesFor(const uint32_t logPages, const uint32_t expectedRecord) {
  const uint64_t logBytes = static_cast<uint64_t>(logPages) * kPageBytes;
  const uint64_t denominator = static_cast<uint64_t>(expectedRecord) * kIndexLoadPercent;
  const uint64_t slots = (logBytes * 100 + denominator - 1) / denominator;
  return static_cast<uint32_t>((slots + kSlotsPerPage - 1) / kSlotsPerPage);
}

}  // namespace

bool planLayout(const PlanInput& in, Layout& out) {
  out = Layout{};
  if (in.styleCount == 0 || in.styleCount > kMaxStyles) return false;

  uint32_t offset = kHeaderRegionBytes;
  uint32_t stylePages = 0;
  uint32_t largestFlash = 0;
  uint32_t sdPages = 0;
  for (uint8_t s = 0; s < in.styleCount; ++s) {
    const uint32_t pages = pagesFor(in.styleBytes[s]);
    const uint8_t mode = in.styleMode[s];
    if (pages == 0 || mode > kModeSd || (mode == kModeFlash && sdPages != 0)) return false;
    out.styleOffset[s] = offset;
    out.styleSize[s] = in.styleBytes[s];
    out.styleMode[s] = mode;
    offset += pages * kPageBytes;
    stylePages += pages;
    if (mode == kModeSd) {
      sdPages += pages;
    } else if (pages > largestFlash) {
      largestFlash = pages;
    }
  }

  const uint32_t usablePages = in.usableBytes / kPageBytes;
  const uint32_t fixedPages = kHeaderRegionBytes / kPageBytes + stylePages;
  const uint32_t mappedStyles = largestFlash + sdPages;
  const uint32_t expected = in.styleMode[0] == kModeSd ? kExpectedRecordSd : kExpectedRecordFlash;

  // From the largest log the partition could hold down: the first that fits both the mapping budget and
  // the partition, together with the index it needs.
  for (uint32_t log = usablePages; log >= kMinLogPages; --log) {
    const uint32_t index = indexPagesFor(log, expected);
    if (mappedStyles + index + kPairPages + log + kSparePages > in.freeMapPages) continue;
    if (fixedPages + index + kPairPages + log > usablePages) continue;
    Geometry& g = out.geometry;
    g.styleCount = in.styleCount;
    g.indexOffset = offset;
    g.indexSlots = index * kSlotsPerPage;
    g.pairOffset = g.indexOffset + index * kPageBytes;
    g.pairSlots = kPairPages * kSlotsPerPage;
    g.logOffset = g.pairOffset + kPairPages * kPageBytes;
    g.logSize = log * kPageBytes;
    return true;
  }
  out = Layout{};
  return false;
}

}  // namespace glyphstore

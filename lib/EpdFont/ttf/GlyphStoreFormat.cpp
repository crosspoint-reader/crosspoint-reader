#include "GlyphStoreFormat.h"

namespace glyphstore {

namespace {

// Index pages so that kIndexLoadPercent of the slots holds logPages of expectedRecord-byte records.
uint32_t indexPagesFor(const uint32_t logPages, const uint32_t expectedRecord) {
  const uint64_t logBytes = static_cast<uint64_t>(logPages) * kPageBytes;
  const uint64_t denominator = static_cast<uint64_t>(expectedRecord) * kIndexLoadPercent;
  const uint64_t slots = (logBytes * 100 + denominator - 1) / denominator;
  const uint64_t pages = (slots + kSlotsPerPage - 1) / kSlotsPerPage;
  return pages == 0 ? 1 : static_cast<uint32_t>(pages);
}

}  // namespace

bool planLayout(const PlanInput& in, Layout& out) {
  out = Layout{};
  if (in.styleCount == 0 || in.styleCount > kMaxStyles) return false;
  if (in.mode != kModeFlash && in.mode != kModeSd) return false;

  uint32_t offset = kHeaderRegionBytes;
  uint32_t stylePages = 0;
  uint32_t largestStyle = 0;
  for (uint8_t s = 0; s < in.styleCount; ++s) {
    const uint32_t pages = pagesFor(in.styleBytes[s]);
    if (pages == 0) return false;
    out.styleOffset[s] = offset;
    out.styleSize[s] = in.styleBytes[s];
    offset += pages * kPageBytes;
    stylePages += pages;
    if (pages > largestStyle) largestStyle = pages;
  }

  const int64_t usablePages = in.usableBytes / kPageBytes;
  const int64_t fixedPages = 1 + static_cast<int64_t>(stylePages);  // the header page and the style blocks
  const int64_t mappedStyles = in.mode == kModeFlash ? largestStyle : stylePages;
  const uint32_t expected = in.mode == kModeFlash ? kExpectedRecordFlash : kExpectedRecordSd;

  // The largest log either limit allows with a one-page index; then shrink it until the index it needs
  // fits as well.
  const int64_t byBudget = static_cast<int64_t>(in.freeMapPages) - mappedStyles - kPairPages - kSparePages - 1;
  const int64_t byPartition = usablePages - fixedPages - kPairPages - 1;
  for (int64_t log = byBudget < byPartition ? byBudget : byPartition; log >= kMinLogPages; --log) {
    const int64_t index = indexPagesFor(static_cast<uint32_t>(log), expected);
    if (mappedStyles + index + kPairPages + log + kSparePages > in.freeMapPages) continue;
    if (fixedPages + index + kPairPages + log > usablePages) continue;
    Geometry& g = out.geometry;
    g.mode = in.mode;
    g.styleCount = in.styleCount;
    g.indexOffset = offset;
    g.indexSlots = static_cast<uint32_t>(index) * kSlotsPerPage;
    g.pairOffset = g.indexOffset + static_cast<uint32_t>(index) * kPageBytes;
    g.pairSlots = kPairPages * kSlotsPerPage;
    g.logOffset = g.pairOffset + kPairPages * kPageBytes;
    g.logSize = static_cast<uint32_t>(log) * kPageBytes;
    return true;
  }
  out = Layout{};
  return false;
}

}  // namespace glyphstore

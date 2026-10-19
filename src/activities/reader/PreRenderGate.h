#pragma once

#include <cstdint>

// The heap the pre-render of the next page needs free before it starts (EpubReaderActivity::renderPreRenderPass): the
// trough the pass must leave, plus half again what it costs. Derived from what the pass consumes rather than from
// what happens to be free -- set the other way twice, it was wrong both times:
//   2026-06-11 (X3): 64 KB silently disabled Background A whenever steady free dipped to ~65 KB (right after a cache
//     rebuild), which also starved Background B behind it. Lowered to 56 KB.
//   2026-08-02 (X3): the scaled-glyph mask cache (~4.75 KB, see GfxRenderer) moved steady free down ~5 KB, so even
//     the post-AA peak (~54.9 KB) fell under 56 KB and A stopped running for entire sessions -- silently, which is
//     why the refusals log.
//
// The trough is 21 KB: a 44 KB floor left 21 KB at the pass's deepest on the X3 (2026-08-02: Page::deserialize
// 10.5 KB, the prewarm's peak transient 8.5 KB), above the ~16 KB the sliced section build reserves. The transient is
// held under RenderLock, so no other reader work allocates into the dip.
//
// The cost is the pass's own measure, the heap's low point while it runs against its start ("cost" in its log line),
// the largest in the book so far, with half again on top: it varies with the fonts and the pages. The X4 (2026-10-18,
// 19 passes): four TrueType families 2.7-6.7 KB, one 4.2-4.7 KB, built-in Bookerly on the same prose 6.7-8.5 KB. The
// X3 (2026-10-19, 10 page turns per mode): built-in Bookerly 5.5-9.9 KB (its prewarm fills the decompressor's page
// slots), one to four TrueType families 4.6-6.4 KB (their glyphs are read from the mapped store). Until a pass is
// measured: 13 KB, half again the X4's 8.5 KB, the fixed allowance this rule replaced -- a 34 KB floor, which refused
// the pass on most pages of a book with publisher fonts on the X3.
//
// A pass that draws glyphs the store does not hold yet pays for their memory too (the scratch block, 10.5-14.5 KB, and
// in SD mode a 4.2 KB page cache), which a miss makes only for itself: 15-26 KB on the X3, right after the glyph store
// was installed again under a chapter laid out before. Such a pass is measured apart, and asked for (without half
// again: that memory does not vary) only while the page drawn last drew such glyphs too; the next one then usually
// does as well. A miss nobody expected finds what it needs or goes without: the page is drawn incomplete and not
// shown.
namespace PreRenderGate {

constexpr uint32_t kTroughBytes = 21 * 1024;
constexpr uint32_t kUnmeasuredAllowanceBytes = 13 * 1024;

// `largestCostBytes`: the most a pass without misses cost; `missCostBytes`: the most one with misses cost, when a miss
// is expected, else 0.
constexpr uint32_t minFreeHeap(const uint32_t largestCostBytes, const uint32_t missCostBytes = 0) {
  const uint32_t allowance =
      largestCostBytes != 0 ? largestCostBytes + largestCostBytes / 2 : kUnmeasuredAllowanceBytes;
  return kTroughBytes + (missCostBytes > allowance ? missCostBytes : allowance);
}

}  // namespace PreRenderGate

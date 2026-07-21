#pragma once

#include <Epub/Page.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "ClippingCodec.h"

class GfxRenderer;

namespace ClippingPageTools {

// Matches the selection activity's definition of a visible token, including
// common Unicode spacing characters emitted by EPUB layout.
bool hasVisibleText(std::string_view text);

// Stable identity for the complete rendered page, not a text-search key. It
// includes the viewport, font identity/metrics, offsets, and every available
// element/line geometry value, so a clipping is highlighted only on the exact
// layout from which it was created. Zero is never returned.
uint32_t fingerprint(const Page& page, const GfxRenderer& renderer, int fontId, int marginLeft, int marginTop);

struct HighlightLine {
  int16_t left = 0;
  int16_t right = 0;
  int16_t y = 0;
};

struct HighlightPlan {
  static constexpr size_t MAX_LINES = 192;
  std::array<HighlightLine, MAX_LINES> lines{};
  size_t count = 0;
  bool truncated = false;

  void draw(GfxRenderer& renderer) const;
};

// Render-task-only, fixed-memory history for the user-facing truncation
// warning. It suppresses repeated popups for recently visited page layouts
// without allocating a set that can grow with the book.
class HighlightNoticeTracker {
 public:
  static constexpr size_t MAX_TRACKED_PAGES = 8;

  bool markIfNew(uint16_t spineIndex, uint16_t pageIndex, uint32_t pageFingerprint) {
    if (pageFingerprint == 0) return false;
    const PageIdentity candidate{spineIndex, pageIndex, pageFingerprint};
    for (size_t i = 0; i < count_; ++i) {
      if (pages_[i] == candidate) return false;
    }
    pages_[next_] = candidate;
    next_ = (next_ + 1) % pages_.size();
    if (count_ < pages_.size()) ++count_;
    return true;
  }

 private:
  struct PageIdentity {
    uint16_t spineIndex = 0;
    uint16_t pageIndex = 0;
    uint32_t pageFingerprint = 0;

    bool operator==(const PageIdentity&) const = default;
  };

  std::array<PageIdentity, MAX_TRACKED_PAGES> pages_{};
  size_t count_ = 0;
  size_t next_ = 0;
};

// Builds at most maxHighlights exact-page selections into fixed geometry once;
// grayscale strip passes can replay it without rescanning words or SD I/O.
HighlightPlan buildExactHighlightPlan(GfxRenderer& renderer, const Page& page, int fontId, int marginLeft,
                                      int marginTop, const std::vector<ClippingCodec::ClippingMetadata>& clippings,
                                      uint16_t spineIndex, uint16_t pageIndex, uint32_t pageFingerprint,
                                      size_t maxHighlights = ClippingCodec::MAX_CLIPPINGS_PER_BOOK);

}  // namespace ClippingPageTools

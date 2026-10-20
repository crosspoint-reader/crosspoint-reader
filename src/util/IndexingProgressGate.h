#pragma once

#include <cstdint>

// When the "Indexing" popup of a book's first open gets its progress bar redrawn, kept free of the
// device so the policy can be tested on the host.
//
// A bar update is a FAST refresh, a few hundred ms on the X3 that the open then waits for (the
// load runs on the SD card, which shares the SPI bus with the panel, so the refresh cannot overlap
// it). Most books index in well under a second and must not pay for one: the bar appears only
// once the popup has been up for kShowAfterMs, at the end of a build phase (OPF, TOC, book.bin),
// and moves at most every kMinIntervalMs. Each move costs ~0.6 s on the X3, so moves closer than
// that interval would cost as much as the work they report: King's Avatar (1732 spine items) ended
// its TOC 3.2 s into the popup and its book.bin 1.2 s later, and the second bar made its popup
// 0.6 s longer (X3, 2026-10-20). It now gets one bar; a book of a few dozen chapters gets none.
class IndexingProgressGate {
 public:
  static constexpr uint32_t kShowAfterMs = 2000;
  static constexpr uint32_t kMinIntervalMs = 2000;

  explicit IndexingProgressGate(const uint32_t popupShownMs) : popupShownMs_(popupShownMs) {}

  // A build phase ended at nowMs with the build `percent` done: true when the bar should be drawn
  // now, and the draw is counted as made.
  bool shouldDraw(const uint32_t nowMs, const uint8_t percent) {
    if (percent <= lastPercent_) return false;
    if (nowMs - popupShownMs_ < kShowAfterMs) return false;
    if (drawn_ && nowMs - lastDrawMs_ < kMinIntervalMs) return false;
    drawn_ = true;
    lastDrawMs_ = nowMs;
    lastPercent_ = percent;
    return true;
  }

  bool drawn() const { return drawn_; }

 private:
  uint32_t popupShownMs_;
  uint32_t lastDrawMs_ = 0;
  uint8_t lastPercent_ = 0;
  bool drawn_ = false;
};

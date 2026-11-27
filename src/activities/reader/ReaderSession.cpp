#include "ReaderSession.h"

#include <algorithm>

namespace {
constexpr uint32_t MIN_DWELL_SECONDS = 2;
constexpr uint32_t MAX_DWELL_SECONDS = 1800;
}  // namespace

uint16_t ReaderSession::clampProgress(const int progressBp) {
  return static_cast<uint16_t>(std::clamp(progressBp, 0, 10000));
}

void ReaderSession::noteTurn(const bool forward, const bool succeeded) { pendingForward = forward && succeeded; }

void ReaderSession::onRenderComplete(const uint32_t monotonicMs, const int64_t trustedEpoch, const int progressBp) {
  const uint16_t progress = clampProgress(progressBp);
  if (pendingForward && hasAnchor && anchorEpoch > 0) {
    const uint32_t elapsedSeconds = (monotonicMs - anchorMs) / 1000;
    if (elapsedSeconds >= MIN_DWELL_SECONDS) {
      const uint32_t contribution = std::min(elapsedSeconds, MAX_DWELL_SECONDS);
      if (duration == 0) {
        startEpoch = anchorEpoch;
        startProgress = anchorProgress;
        endProgress = anchorProgress;
      }
      duration += contribution;
      endProgress = std::max(endProgress, progress);
    }
  }
  // Turning onto the end finishes the session's book even when the last turn was quick: the
  // dwell rule times reading, it should not hide the finish. A jump there is not reading it.
  if (pendingForward && progress == 10000 && startEpoch > 0) endProgress = 10000;

  anchorMs = monotonicMs;
  anchorEpoch = trustedEpoch;
  anchorProgress = progress;
  hasAnchor = true;
  pendingForward = false;
}

void ReaderSession::reset() {
  anchorMs = 0;
  anchorEpoch = 0;
  startEpoch = 0;
  duration = 0;
  anchorProgress = 0;
  startProgress = 0;
  endProgress = 0;
  hasAnchor = false;
  pendingForward = false;
}

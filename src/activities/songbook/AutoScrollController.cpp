#include "AutoScrollController.h"

#include <algorithm>

namespace songbook {

void AutoScrollController::configure(const std::vector<int32_t>& stops, const int32_t contentHeight,
                                     const int viewHeight, const uint32_t durationMs) {
  duration = durationMs;
  stepTimes.clear();
  stepTimes.reserve(std::max<size_t>(stops.size(), 1));
  stepTimes.push_back(0);
  const uint64_t lead = viewHeight > 0 ? static_cast<uint64_t>(viewHeight) / 3 : 0;
  for (size_t k = 1; k < stops.size() && contentHeight > 0; ++k) {
    const uint64_t readTo = static_cast<uint64_t>(std::max<int32_t>(stops[k], 0)) + lead;
    uint64_t due = static_cast<uint64_t>(duration) * readTo / static_cast<uint64_t>(contentHeight);
    // Strictly increasing keeps stepForElapsed() exact for every stop.
    due = std::max<uint64_t>(due, static_cast<uint64_t>(stepTimes.back()) + 1);
    stepTimes.push_back(static_cast<uint32_t>(std::min<uint64_t>(due, UINT32_MAX)));
  }
  stop();
}

size_t AutoScrollController::stepForElapsed(const uint32_t elapsed) const {
  const auto it = std::upper_bound(stepTimes.begin(), stepTimes.end(), elapsed);
  return it == stepTimes.begin() ? 0 : static_cast<size_t>(it - stepTimes.begin()) - 1;
}

uint32_t AutoScrollController::elapsedMs(const uint32_t nowMs) const {
  if (currentState != State::Playing) return accumulated;
  // The signed difference stays correct across the millis() wrap; a time
  // read before the run started (a caller's stale timestamp) counts as zero.
  const int32_t run = static_cast<int32_t>(nowMs - runStart);
  const uint64_t total = static_cast<uint64_t>(accumulated) + static_cast<uint32_t>(run > 0 ? run : 0);
  return static_cast<uint32_t>(std::min<uint64_t>(total, duration));
}

void AutoScrollController::start(const uint32_t nowMs) {
  if (currentState == State::Finished) currentStep = 0;
  accumulated = stepTimeMs(currentStep);
  runStart = nowMs;
  currentState = State::Playing;
}

void AutoScrollController::pause(const uint32_t nowMs) {
  if (currentState != State::Playing) return;
  accumulated = elapsedMs(nowMs);
  currentState = State::Paused;
}

void AutoScrollController::resume(const uint32_t nowMs) {
  if (currentState != State::Paused) return;
  runStart = nowMs;
  currentState = State::Playing;
}

void AutoScrollController::stop() {
  currentState = State::Idle;
  currentStep = 0;
  accumulated = 0;
}

bool AutoScrollController::update(const uint32_t nowMs) {
  if (currentState != State::Playing) return false;
  const uint32_t elapsed = elapsedMs(nowMs);
  if (elapsed >= duration) {
    // Stay on the last stop; the next song is never opened automatically.
    accumulated = duration;
    currentStep = stepTimes.size() - 1;
    currentState = State::Finished;
    return true;
  }
  const size_t due = stepForElapsed(elapsed);
  if (due == currentStep) return false;
  currentStep = due;
  return true;
}

bool AutoScrollController::pending(const uint32_t nowMs) const {
  if (currentState != State::Playing) return false;
  const uint32_t elapsed = elapsedMs(nowMs);
  return elapsed >= duration || stepForElapsed(elapsed) != currentStep;
}

bool AutoScrollController::stepBy(const int delta, const uint32_t nowMs) {
  const int last = static_cast<int>(stepTimes.size()) - 1;
  const int target = std::clamp(static_cast<int>(currentStep) + delta, 0, last);
  if (target == static_cast<int>(currentStep)) return false;
  currentStep = static_cast<size_t>(target);
  switch (currentState) {
    case State::Playing:
      accumulated = stepTimes[currentStep];
      runStart = nowMs;
      break;
    case State::Paused:
      accumulated = stepTimes[currentStep];
      break;
    case State::Finished:
      // Scrolling back after the end is browsing, not a resumed performance.
      currentState = State::Idle;
      accumulated = 0;
      break;
    case State::Idle:
      break;
  }
  return true;
}

}  // namespace songbook

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace songbook {

// Performance timer for the lyrics view. Position is derived from the elapsed
// play time on every update, so slow e-ink refreshes never accumulate drift.
// Step k is due when a reader moving at a constant pace through the song
// (contentHeight over durationMs) reaches 1/3 below stop k's top, i.e. when
// they have read 2/3 of the screen. The last stop stays up until the end.
class AutoScrollController {
 public:
  enum class State : uint8_t { Idle, Playing, Paused, Finished };

  // Resets to Idle at the first stop. Times are in a monotonic millis() base.
  void configure(const std::vector<int32_t>& stops, int32_t contentHeight, int viewHeight, uint32_t durationMs);

  // Starts from the current stop (from the top after Finished).
  void start(uint32_t nowMs);
  void pause(uint32_t nowMs);
  void resume(uint32_t nowMs);
  // Back to Idle at the top.
  void stop();
  // Returns true when the step or state changed.
  bool update(uint32_t nowMs);
  // True when update(nowMs) would change something; has no side effects.
  bool pending(uint32_t nowMs) const;
  // Manual step; while playing or paused the timeline jumps to that step's
  // due time so later steps keep their spacing. Returns true if it moved.
  bool stepBy(int delta, uint32_t nowMs);

  State state() const { return currentState; }
  bool active() const { return currentState == State::Playing || currentState == State::Paused; }
  size_t step() const { return currentStep; }
  size_t stepCount() const { return stepTimes.size(); }
  uint32_t durationMs() const { return duration; }
  uint32_t elapsedMs(uint32_t nowMs) const;
  uint32_t stepTimeMs(const size_t index) const { return index < stepTimes.size() ? stepTimes[index] : duration; }

 private:
  size_t stepForElapsed(uint32_t elapsed) const;

  std::vector<uint32_t> stepTimes;
  uint32_t duration = 0;
  // Play time banked before the current run; the run adds now - runStart.
  uint32_t accumulated = 0;
  uint32_t runStart = 0;
  size_t currentStep = 0;
  State currentState = State::Idle;
};

}  // namespace songbook

#pragma once

#include <cstdint>

// The decision behind the serial CMD:IDLE (SerialTestApi.cpp), kept free of the device so it can be
// tested on the host. The caller samples the device once per main-loop pass and passes the sample
// to step().
//
// Idle needs more than "nothing looks busy right now":
//  - Quiet must last QUIET_MS. The steps of one piece of work leave gaps between them (a press is
//    taken in on one pass, its render queued at the end of that pass).
//  - A command that should change the screen (a button press) is an EFFECT. Until the input path has
//    handled it, the device is busy whatever else it says: a press can still be on its way while
//    nothing looks busy. On the pass that handles it, work already under way is the effect's, and
//    the normal busy check follows it from there. If nothing is under way yet, the effect is done
//    once a render pass starts, or once NO_RENDER_GRACE_MS pass without one (the press changed
//    nothing on screen).
class IdleGate {
 public:
  static constexpr uint32_t QUIET_MS = 250;
  static constexpr uint32_t NO_RENDER_GRACE_MS = 1000;

  enum class Verdict : uint8_t { Waiting, Idle, TimedOut, PressDropped };

  struct Sample {
    uint32_t nowMs = 0;
    const char* busy = nullptr;   // the device's own busy reason, nullptr when it looks idle
    bool effectHandled = true;    // the input path is done with the last effect
    uint32_t renderPasses = 0;    // render passes started so far
    uint32_t droppedPresses = 0;  // presses given up so far (ButtonPressQueue::droppedPresses())
    uint32_t takenPresses = 0;    // presses taken in so far (ButtonPressQueue::takenPresses())
  };

  // A command that should change the screen ran; `takenPresses` as in Sample, at that moment.
  void noteEffect(const uint32_t takenPresses) {
    if (!effectPending) effectTakenBase = takenPresses;  // an effect already followed keeps its own
    effectPending = true;
    effectHandledSeen = false;
  }

  // Starts a wait (CMD:IDLE). False while one is running, which goes on unchanged: each wait gets
  // its own answer.
  bool begin(const uint32_t nowMs, const uint32_t limitMs) {
    if (waiting) return false;
    waiting = true;
    startMs = nowMs;
    timeoutMs = limitMs;
    quiet = false;
    lastBusy = "settling";
    return true;
  }

  bool isWaiting() const { return waiting; }
  // A wait is running or an effect is being followed: step() has something to do.
  bool active() const { return waiting || effectPending; }
  uint32_t elapsedMs(const uint32_t nowMs) const { return nowMs - startMs; }
  // What last kept the device busy; "settling" when nothing did but quiet was not long enough yet.
  const char* reason() const { return lastBusy; }

  // Called every pass, wait or not, so an effect is followed from the command on.
  Verdict step(const Sample& sample) {
    const char* busy = effectBusy(sample);
    if (!busy) busy = sample.busy;
    if (!waiting) return Verdict::Waiting;
    if (busy) {
      quiet = false;
      lastBusy = busy;
    } else if (!quiet) {
      quiet = true;
      quietSinceMs = sample.nowMs;
    }
    // The deadline wins: a pass that comes late (a stalled loop) must not make an expired wait Idle.
    const bool expired = sample.nowMs - startMs >= timeoutMs;
    const bool settled = quiet && sample.nowMs - quietSinceMs >= QUIET_MS;
    if (!expired && !settled) return Verdict::Waiting;
    waiting = false;
    // A press given up since the last answer never reached the screen, whatever it shows now.
    if (sample.droppedPresses != droppedAnswered) {
      droppedAnswered = sample.droppedPresses;
      // No press left to play and none taken in: no frame can follow, so the next wait must not
      // wait for one. A press taken in may still draw (a deferred single click).
      if (sample.effectHandled && sample.takenPresses == effectTakenBase) effectPending = false;
      return Verdict::PressDropped;
    }
    return expired ? Verdict::TimedOut : Verdict::Idle;
  }

 private:
  const char* effectBusy(const Sample& sample) {
    if (!effectPending) return nullptr;
    if (!sample.effectHandled) return "input";
    if (!effectHandledSeen) {
      effectHandledSeen = true;
      if (sample.busy) {  // its work is under way: the busy check follows it from here
        effectPending = false;
        return nullptr;
      }
      effectHandledAtMs = sample.nowMs;
      effectRenderPasses = sample.renderPasses;
      return "render";
    }
    if (sample.renderPasses != effectRenderPasses || sample.nowMs - effectHandledAtMs >= NO_RENDER_GRACE_MS) {
      effectPending = false;
      return nullptr;
    }
    return "render";
  }

  bool waiting = false;
  bool quiet = false;
  uint32_t startMs = 0;
  uint32_t timeoutMs = 0;
  uint32_t quietSinceMs = 0;
  const char* lastBusy = "settling";
  uint32_t droppedAnswered = 0;  // droppedPresses at the last answer

  bool effectPending = false;
  bool effectHandledSeen = false;
  uint32_t effectTakenBase = 0;  // takenPresses when the effect began
  uint32_t effectRenderPasses = 0;
  uint32_t effectHandledAtMs = 0;
};

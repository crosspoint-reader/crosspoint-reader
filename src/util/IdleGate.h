#pragma once

#include <cstdint>

// The decision behind the serial CMD:IDLE (see SerialTestApi), kept free of the device so the
// sequencing can be tested on the host. The caller samples the device once per main-loop pass and
// hands the sample to step().
//
// Two things make "idle" more than "nothing looks busy right now":
//  - Quiet must last kQuietMs. The steps of one piece of work leave gaps between them (a press is
//    taken in on the next pass, its render queued at the end of that pass).
//  - A command that should change the screen (a press, a touch, a screen switch) is an EFFECT. Until
//    the input path has handled it, the device is busy whatever else it says: a press can sit
//    queued while nothing looks busy, and IDLE answered before the page turned. On the pass that
//    handles it, work already under way (a frame queued or drawing) is the effect's, and the
//    normal busy check follows it from there. If nothing is under way yet, the effect is done once
//    a render pass starts after that pass, or once kNoRenderGraceMs pass without one (the press
//    changed nothing on screen).
class IdleGate {
 public:
  static constexpr uint32_t kQuietMs = 250;
  static constexpr uint32_t kNoRenderGraceMs = 1000;

  enum class Verdict : uint8_t { Waiting, Idle, TimedOut };

  struct Sample {
    uint32_t nowMs = 0;
    const char* busy = nullptr;  // the device's own busy reason, nullptr when it looks idle
    bool effectHandled = true;   // the input path is done with the last effect
    uint32_t renderPasses = 0;   // render passes started so far
  };

  // A command that should change the screen ran.
  void noteEffect() {
    effectPending_ = true;
    effectHandledSeen_ = false;
  }

  // Starts a wait (CMD:IDLE); a wait already running is replaced.
  void begin(const uint32_t nowMs, const uint32_t timeoutMs) {
    waiting_ = true;
    startMs_ = nowMs;
    timeoutMs_ = timeoutMs;
    quiet_ = false;
    lastBusy_ = "settling";
  }

  bool waiting() const { return waiting_; }
  // A wait is running or an effect is being followed: step() has something to do.
  bool active() const { return waiting_ || effectPending_; }
  uint32_t elapsedMs(const uint32_t nowMs) const { return nowMs - startMs_; }
  // What last kept the device busy; "settling" when nothing did but quiet was not long enough yet.
  const char* reason() const { return lastBusy_; }

  // Called every pass, wait or not, so an effect is followed from the command on.
  Verdict step(const Sample& sample) {
    const char* busy = effectBusy(sample);
    if (!busy) busy = sample.busy;
    if (!waiting_) return Verdict::Waiting;
    if (busy) {
      quiet_ = false;
      lastBusy_ = busy;
    } else if (!quiet_) {
      quiet_ = true;
      quietSinceMs_ = sample.nowMs;
    }
    if (quiet_ && sample.nowMs - quietSinceMs_ >= kQuietMs) {
      waiting_ = false;
      return Verdict::Idle;
    }
    if (sample.nowMs - startMs_ >= timeoutMs_) {
      waiting_ = false;
      return Verdict::TimedOut;
    }
    return Verdict::Waiting;
  }

 private:
  const char* effectBusy(const Sample& sample) {
    if (!effectPending_) return nullptr;
    if (!sample.effectHandled) return "input";
    if (!effectHandledSeen_) {
      effectHandledSeen_ = true;
      if (sample.busy) {  // its work is under way: the busy check follows it from here
        effectPending_ = false;
        return nullptr;
      }
      effectHandledAtMs_ = sample.nowMs;
      effectRenderPasses_ = sample.renderPasses;
      return "render";
    }
    if (sample.renderPasses != effectRenderPasses_ || sample.nowMs - effectHandledAtMs_ >= kNoRenderGraceMs) {
      effectPending_ = false;
      return nullptr;
    }
    return "render";
  }

  bool waiting_ = false;
  bool quiet_ = false;
  uint32_t startMs_ = 0;
  uint32_t timeoutMs_ = 0;
  uint32_t quietSinceMs_ = 0;
  const char* lastBusy_ = "settling";

  bool effectPending_ = false;
  bool effectHandledSeen_ = false;
  uint32_t effectRenderPasses_ = 0;
  uint32_t effectHandledAtMs_ = 0;
};

#pragma once
#include <cstdint>

// Advance once per firmware loop; repeated GPIO polls retain the same edges.
class SerialInput {
  enum class Phase : uint8_t { Idle, Pending, Down, Releasing, Released };
  Phase phase = Phase::Idle;
  uint8_t buttonMask = 0;
  bool pressedEdge = false;
  uint32_t startedAt = 0;
  uint32_t duration = 0;
  uint32_t releasedAfter = 0;

 public:
  bool start(uint8_t button, uint32_t holdMs) {
    if (active() || button >= 7 || holdMs < 20 || holdMs > 2000) return false;
    buttonMask = 1u << button;
    duration = holdMs;
    phase = Phase::Pending;
    return true;
  }
  void beginFrame(uint32_t now) {
    pressedEdge = false;
    if (phase == Phase::Released) cancel();
    if (phase == Phase::Pending) {
      phase = Phase::Down;
      startedAt = now;
      pressedEdge = true;
    } else if ((phase == Phase::Down && now - startedAt >= duration) || phase == Phase::Releasing) {
      releasedAfter = now - startedAt;
      phase = Phase::Released;
    }
  }
  // Ends a hold early: a held button releases on the next frame so press/release
  // stays paired. Returns true when a release will be emitted.
  bool releaseEarly() {
    if (phase == Phase::Down) {
      phase = Phase::Releasing;
      return true;
    }
    if (phase == Phase::Pending) cancel();
    return false;
  }
  void cancel() {
    phase = Phase::Idle;
    buttonMask = 0;
    pressedEdge = false;
  }
  bool active() const { return phase != Phase::Idle; }
  uint8_t down() const { return phase == Phase::Down || phase == Phase::Releasing ? buttonMask : 0; }
  uint8_t pressed() const { return pressedEdge ? buttonMask : 0; }
  uint8_t released() const { return phase == Phase::Released ? buttonMask : 0; }
  uint32_t heldMs(uint32_t now) const { return released() ? releasedAfter : down() ? now - startedAt : 0; }
};

#ifdef ENABLE_SERIAL_LOG
inline SerialInput serialInput;
#endif

#pragma once

// Pure, header-only state machine for arbitrating a capacitive Home-key double
// click. Feeds on one main-loop frame at a time while a first tap is pending.
struct HomeTapTracker {
  bool armed = false;
  unsigned long armedAt = 0;

  void arm(unsigned long now) {
    armed = true;
    armedAt = now;
  }

  void disarm() { armed = false; }

  enum class Step { None, DoubleClick, WindowExpired };

  // Feed one main-loop frame while a tap is pending. Returns what to do.
  Step update(bool secondTapSeen, unsigned long now, unsigned long windowMs) {
    if (!armed) return Step::None;
    if (secondTapSeen) {
      disarm();
      return Step::DoubleClick;
    }
    if (now - armedAt >= windowMs) {
      disarm();
      return Step::WindowExpired;
    }
    return Step::None;
  }
};

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
  // Expiry is checked BEFORE honoring a second tap: if a loop stall delays this
  // call past windowMs, a tap that arrives "late" must not be misread as the
  // second click of an expired pair — it starts a fresh window instead.
  Step update(bool secondTapSeen, unsigned long now, unsigned long windowMs) {
    if (!armed) return Step::None;
    if (now - armedAt >= windowMs) {
      disarm();
      return Step::WindowExpired;
    }
    if (secondTapSeen) {
      disarm();
      return Step::DoubleClick;
    }
    return Step::None;
  }
};

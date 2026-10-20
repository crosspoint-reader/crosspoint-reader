#pragma once

// The USB verdict the firmware acts on -- main.cpp repaints the screen on an edge, idle light sleep
// is refused while it reads attached -- from the raw reading HalGPIO::updateUsbState() takes every
// loop pass (the IDF's SOF-based host link OR'd with the board's electrical check).
//
// The raw reading drops out for a few milliseconds now and then while a host stays attached. On the
// X3 the host link read absent for 12-46 ms at a time, always while the CPU sat at 10 MHz in a
// display waveform wait (device-logged, 2026-10-20). Taken as it came, each dropout was an unplug
// and a re-plug: two repaints of the page on screen, and the first, landing in a page turn, also
// cost that turn its anti-aliasing (aaPreemptedByNavigation() reads any queued update as a page on
// its way out). It would also let the idle loop light-sleep for a slice, and light sleep kills an
// enumerated CDC link for real.
//
// So a detach counts only once the reading has stayed absent for kDetachHoldMs. An attach counts at
// once: a fresh enumeration must cancel light sleep within a poll or two, or the next slice kills
// the CDC link again.
class UsbLinkDebounce {
 public:
  // Ten times the longest dropout seen, and short enough that the battery icon follows an unplug
  // within a glance. Idle light sleep waits a second of inactivity anyway.
  static constexpr unsigned long kDetachHoldMs = 500;

  // One raw reading per call. Returns true when the verdict changed with it.
  bool update(const bool rawConnected, const unsigned long nowMs) {
    if (rawConnected) {
      absent_ = false;
      return report(true);
    }
    if (!absent_) {
      absent_ = true;
      absentSinceMs_ = nowMs;
    }
    if (nowMs - absentSinceMs_ < kDetachHoldMs) {
      return false;
    }
    return report(false);
  }

  bool connected() const { return connected_; }

 private:
  bool report(const bool value) {
    const bool changed = value != connected_;
    connected_ = value;
    return changed;
  }

  bool connected_ = false;
  bool absent_ = false;
  unsigned long absentSinceMs_ = 0;
};

#pragma once

#include <BlePageTurner.h>

// Whether a chapter build may run next to the page turner's radio. A radio that did not stop in
// time, is still stopping, or is still starting holds or is taking the heap the build needs, and
// a build that runs out of memory aborts the firmware (no exceptions). A radio refused for memory
// holds nothing.
inline bool chapterBuildMayRun(const bleturner::Status& radio) {
  return !radio.running && !radio.stopping && !radio.starting;
}

// Asks `ready` until it says yes, sleeping stepMs between asks, and gives up once waitMs has
// passed after the first ask returned (that ask may itself wait: the module's stop takes up to
// 6 s). now and sleep are the host's clock (millis, delay); tests pass their own.
template <typename Ready, typename Now, typename Sleep>
bool waitForBuildRoom(Ready ready, Now now, Sleep sleep, const uint32_t waitMs, const uint32_t stepMs) {
  if (ready()) return true;
  const uint32_t started = now();
  do {
    if (now() - started >= waitMs) return false;
    sleep(stepMs);
  } while (!ready());
  return true;
}

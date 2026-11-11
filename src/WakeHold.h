#pragma once

// "Always Long Press to Wake": a wake from the power button has to be a deliberate hold, so a bump
// in a bag or pocket cannot wake the device. Wakes from anything else, and the restarts the
// firmware does on its own, are not button presses and are never held up.
constexpr unsigned long kWakeHoldMs = 500;

constexpr bool wakeNeedsLongPress(const bool settingOn, const bool powerButtonWake, const bool silentReboot) {
  return settingOn && powerButtonWake && !silentReboot;
}

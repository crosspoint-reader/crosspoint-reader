#pragma once

#include <HalGPIO.h>

#include "CrossPointSettings.h"

// A slide switch owns the state of the function it is bound to, so the UI must
// not offer its own toggle for that function or change it behind the switch.
inline bool slideSwitchOwns(const uint8_t action) {
  return gpio.hasToggleSwitch() && SETTINGS.slideSwitchAction == action;
}

inline bool slideSwitchOwnsLight() { return slideSwitchOwns(CrossPointSettings::SLIDE_SWITCH_FRONTLIGHT); }

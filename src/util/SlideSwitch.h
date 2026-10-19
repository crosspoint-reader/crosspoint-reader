#pragma once

#include <HalGPIO.h>

#include "CrossPointSettings.h"

// A slide switch bound to the frontlight owns its on/off state, so the UI must
// not offer its own light toggle or switch the light on behind the switch.
inline bool slideSwitchOwnsLight() {
  return gpio.hasToggleSwitch() && SETTINGS.slideSwitchAction == CrossPointSettings::SLIDE_SWITCH_FRONTLIGHT;
}

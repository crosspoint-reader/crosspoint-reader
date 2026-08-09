// Frontlight brightness, warmth and its single persisted write.

#include <HalFrontlight.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "internal.h"

extern "C" {

uint8_t cpp_frontlight_present() { return Frontlight.present() ? 1 : 0; }

uint8_t cpp_frontlight_has_color_temperature() { return Frontlight.hasColorTemperature() ? 1 : 0; }

int32_t cpp_frontlight_brightness() { return Frontlight.brightness(); }

int32_t cpp_frontlight_warmth() { return Frontlight.warmth(); }

uint8_t cpp_frontlight_is_on() { return Frontlight.isOn() ? 1 : 0; }

void cpp_frontlight_set_brightness(const int32_t percent) {
  Frontlight.setBrightness(static_cast<uint8_t>(std::clamp<int32_t>(percent, 0, 100)));
}

void cpp_frontlight_set_warmth(const int32_t percent) {
  Frontlight.setWarmth(static_cast<uint8_t>(std::clamp<int32_t>(percent, 0, 100)));
}

void cpp_frontlight_set_on(const uint8_t on) { Frontlight.setOn(on != 0); }

void cpp_frontlight_save() {
  // Guarded so a caller cannot wear the flash out by saving on every
  // adjustment: SPIFFS sectors have a finite erase budget.
  const uint8_t brightness = Frontlight.brightness();
  const uint8_t warmth = Frontlight.warmth();
  const uint8_t on = Frontlight.isOn() ? 1 : 0;

  if (SETTINGS.frontlightBrightness == brightness && SETTINGS.frontlightWarmth == warmth &&
      SETTINGS.frontlightOn == on) {
    return;
  }

  SETTINGS.frontlightBrightness = brightness;
  SETTINGS.frontlightWarmth = warmth;
  SETTINGS.frontlightOn = on;
  SETTINGS.saveToFile();
}
}

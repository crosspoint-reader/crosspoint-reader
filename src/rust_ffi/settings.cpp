// Persisted settings Rust screens read or write. Deliberately narrow: a general
// settings writer would hand every screen the ability to write flash.

#include <HalDisplay.h>

#include "CrossPointSettings.h"
#include "internal.h"

extern "C" {

uint8_t cpp_display_is_inverted() { return display.isInverted() ? 1 : 0; }

uint8_t cpp_display_toggle_inverted() {
  const bool inverted = display.toggleInverted();
  SETTINGS.screenInverted = inverted ? 1 : 0;
  SETTINGS.saveToFile();
  return inverted ? 1 : 0;
}
}

#pragma once

// Host-build stand-in for freeink-sdk's BoardConfig.h (real one pulls in
// <driver/gpio.h>/<esp_rom_sys.h>, ESP-IDF only). GfxRenderer.cpp only reads
// BoardConfig::ACTIVE.viewableInsets — defaults match the real X4 profile's
// bezel insets (see BoardConfig.h's ViewableInsets comment).
#include <cstdint>

struct BoardConfigViewableInsets {
  uint8_t top;
  uint8_t right;
  uint8_t bottom;
  uint8_t left;
};

struct BoardConfigProfile {
  BoardConfigViewableInsets viewableInsets;
};

class BoardConfig {
 public:
  using ViewableInsets = BoardConfigViewableInsets;
  using Profile = BoardConfigProfile;
  static Profile ACTIVE;
};

// Out-of-class definition: ViewableInsets/Profile are unrelated free
// structs (not nested), so this avoids the "default member initializer
// required before end of enclosing class" ordering error GCC raises for
// an inline static member of a nested type defined inside the same class.
inline BoardConfig::Profile BoardConfig::ACTIVE = {{9, 3, 3, 3}};

#pragma once

// Host-build stand-in for lib/hal/HalGPIO.h. GfxRenderer.cpp includes this
// but (grep-verified) never calls anything on the global `gpio` — it only
// needs the header to parse. The real header also includes <Arduino.h>
// (for millis(), used elsewhere in GfxRenderer.cpp) — mirrored here so this
// stub is a drop-in replacement.
#include "Arduino.h"

class HalGPIO {
 public:
  bool hasTouch() const { return false; }
  bool hasEdgeSideButtons() const { return false; }
  bool isUsbConnected() const { return false; }
  bool deviceIsX3() const { return false; }
};

extern HalGPIO gpio;

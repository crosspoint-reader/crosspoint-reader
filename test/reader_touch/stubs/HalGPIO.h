#pragma once

struct HalGPIO {
  unsigned long heldMs = 0;
  unsigned long lastTouchHeldMs() const { return heldMs; }
};

inline HalGPIO gpio;

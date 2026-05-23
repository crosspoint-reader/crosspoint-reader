#include "CrossPointInfo.h"

#include <HalDisplay.h>
#include <HalGPIO.h>

#include <array>
#include <cassert>
#include <cstdio>

const char* getCrossPointHttpUserAgent() {
  // Thread-safe, one-time initialization guaranteed by C++ local static init.
  // 80 bytes comfortably fits the longest expected UA string:
  // "CrossPoint/1.3.0-dev+abc1234 (X3; esp32-c3; 528x800)" (~55 chars)
  static const auto ua = []() {
    std::array<char, 80> buf{};
    const char* model = gpio.deviceIsX3() ? "X3" : "X4";
    const int len =
        snprintf(buf.data(), buf.size(), "CrossPoint/" CROSSPOINT_VERSION " (%s; " CROSSPOINT_MCU "; %ux%u)", model,
                 display.getDisplayWidth(), display.getDisplayHeight());
    assert(len > 0 && len < static_cast<int>(buf.size()));
    return buf;
  }();
  return ua.data();
}

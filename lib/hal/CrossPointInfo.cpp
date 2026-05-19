#include "CrossPointInfo.h"

#include <HalDisplay.h>
#include <HalGPIO.h>

#include <cstdio>

const char* getCrossPointHttpUserAgent() {
  // 80 bytes comfortably fits the longest expected UA string:
  // "CrossPoint/1.3.0-dev+abc1234 (X3; esp32-c3; 528x800)" (~55 chars)
  static char buf[80];
  static bool built = false;
  if (built) return buf;

  const char* model = gpio.deviceIsX3() ? "X3" : "X4";

  snprintf(buf, sizeof(buf), "CrossPoint/" CROSSPOINT_VERSION " (%s; " CROSSPOINT_MCU "; %ux%u)", model,
           display.getDisplayWidth(), display.getDisplayHeight());
  built = true;
  return buf;
}

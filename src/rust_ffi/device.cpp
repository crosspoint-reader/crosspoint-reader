// Device identity and battery.

#include <BoardConfig.h>
#include <HalPowerManager.h>

#include "internal.h"

extern "C" {

const uint8_t* cpp_device_firmware_version() { return reinterpret_cast<const uint8_t*>(CROSSPOINT_VERSION); }

const uint8_t* cpp_device_name() {
  // BoardConfig is the only source that separates an X4 Pro from a plain X4.
  return reinterpret_cast<const uint8_t*>(BoardConfig::ACTIVE.name);
}

int32_t cpp_device_battery_percent() { return powerManager.getBatteryPercentage(); }
}

// Host stand-in for the SDK's BoardConfig.h: the fields of the active profile HalStorage.cpp reads.
#pragma once
#include <cstdint>

namespace BoardConfig {
struct SdmmcPins {
  uint8_t busWidth = 0;  // 0: the card is on the SPI bus, as on the C3 boards
};
struct SdPins {
  uint32_t spiHz = 0;
};
struct Profile {
  const char* name = "host";
  SdmmcPins sdmmc;
  SdPins sd;
};
inline Profile ACTIVE;
}  // namespace BoardConfig

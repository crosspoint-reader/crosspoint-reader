#pragma once
#include <cstdint>
namespace BoardConfig {
struct Profile {
  uint16_t displayWidth = 416;
  uint16_t displayHeight = 240;
  uint32_t displaySpiHz = 4000000;
};
inline Profile ACTIVE;
}

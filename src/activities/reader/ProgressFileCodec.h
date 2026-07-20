#pragma once

#include <cstdint>

namespace ProgressFileCodec {

inline void encodePage(const uint32_t page, uint8_t (&data)[4]) {
  data[0] = static_cast<uint8_t>(page);
  data[1] = static_cast<uint8_t>(page >> 8);
  data[2] = static_cast<uint8_t>(page >> 16);
  data[3] = static_cast<uint8_t>(page >> 24);
}

inline uint32_t decodePage(const uint8_t (&data)[4]) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

}  // namespace ProgressFileCodec

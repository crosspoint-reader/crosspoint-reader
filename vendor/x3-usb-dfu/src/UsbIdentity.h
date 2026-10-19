#pragma once
#include <cstddef>
#include <cstdint>

namespace usb_update {
// Set before USB starts; descriptor callbacks run only on the USB worker.
inline bool identityIsX3 = true;
inline const uint16_t* stringDescriptor(uint8_t index, uint16_t language) {
  static uint16_t descriptor[32];
  if (index == 0) {
    descriptor[0] = 0x0304;
    descriptor[1] = 0x0409;
    return descriptor;
  }
  if (language != 0x0409 || index > 2) return nullptr;
  const char* text = index == 1 ? "CrossPoint" : identityIsX3 ? "CrossPoint X3 USB Update" : "CrossPoint X4 USB Update";
  size_t length = 0;
  while (text[length] && length < 31) {
    descriptor[length + 1] = static_cast<uint8_t>(text[length]);
    ++length;
  }
  descriptor[0] = static_cast<uint16_t>(0x0300 | (2 * (length + 1)));
  return descriptor;
}
}  // namespace usb_update

#pragma once
#include <cstdint>

#include "reply_policy.h"

namespace usb_update {
struct PipeResult {
  uint8_t wire[68];
  uint32_t start, se0, j, ready, count, error, crc;
};
static_assert(sizeof(PipeResult) == 96);
// One allocation per connection avoids permanent packet-buffer reservations
// and avoids fragmentation from separately allocating each small USB table.
struct UsbWorkspace {
  uint16_t crc[256]{}, pidMeta[16]{};
  uint8_t tokenCrc[2048]{};
  PipeResult results[2]{};
  su_reply_policy policy{};
  su_device device{};
  alignas(4) uint8_t transfer[4096]{};
};
static_assert(sizeof(UsbWorkspace) < 11000);
}  // namespace usb_update

#pragma once
#include "FirmwareFlasher.h"
namespace usb_update {
// Writes only the inactive application slot. Selecting it is a separate final
// operation after the entire stored image passes format and SHA validation.
class FlashWriter {
  const esp_partition_t* destination = nullptr;
  uint32_t declared = 0, written = 0, erased = 0;
  uint8_t expected[32]{};

 public:
  firmware_flash::Result result = firmware_flash::Result::OPEN_FAIL;
  uint32_t eraseMs = 0, writeMs = 0, verifyMs = 0, selectMs = 0, eraseCount = 0, writeCount = 0, maxEraseMs = 0,
           maxWriteMs = 0;
  bool begin(const esp_partition_t*, uint32_t, const uint8_t[32]);
  bool append(const uint8_t*, size_t);
  bool finish();
  void abort() { destination = nullptr; }
  uint32_t bytes() const { return written; }
  bool needsErase(size_t n) const { return written + n > erased; }
};
}  // namespace usb_update

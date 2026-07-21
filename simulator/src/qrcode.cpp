#include "qrcode.h"

#include <cstdint>

// Minimal stubs so QrUtils compiles and links in the simulator.
// Actual QR rendering is not needed for simulator functionality.

uint32_t qrcode_getBufferSize(uint8_t version) {
  // Formula from the real library: ((version * 4 + 17) * (version * 4 + 17) +
  // 7) / 8 + 1
  uint8_t size = version * 4 + 17;
  return ((uint32_t)size * size + 7) / 8 + 1;
}

int8_t qrcode_initText(QRCode *qrcode, uint8_t * /*modules*/, uint8_t version,
                       QrCodeEcc /*ecc*/, const char * /*data*/) {
  if (qrcode) {
    qrcode->size = version * 4 + 17;
  }
  return 0;
}

int qrcode_getModule(QRCode * /*qrcode*/, uint8_t /*x*/, uint8_t /*y*/) {
  return 0;
}

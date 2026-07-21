#pragma once

#include <cstdint>

// Dummy QR code library for simulator.
enum QrCodeEcc { ECC_LOW = 0, ECC_MEDIUM, ECC_QUARTILE, ECC_HIGH };

class QRCode {
public:
  uint8_t size;
  QRCode() : size(0) {}
};

uint32_t qrcode_getBufferSize(uint8_t version);
int8_t qrcode_initText(QRCode *qrcode, uint8_t *modules, uint8_t version,
                       QrCodeEcc ecc, const char *data);
int qrcode_getModule(QRCode *qrcode, uint8_t x, uint8_t y);

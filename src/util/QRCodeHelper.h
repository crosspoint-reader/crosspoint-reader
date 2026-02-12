#pragma once

#include <GfxRenderer.h>
#include <qrcode.h>

#include <string>

/**
 * Shared QR code rendering utility.
 * Renders a Version 4 QR code (33 modules) with ECC_LOW on an e-ink display.
 */
namespace QRCodeHelper {

/// Version 4 QR code = 33 modules per side
constexpr uint8_t QR_MODULES = 33;

/// Default pixels per QR module (same as File Transfer)
constexpr uint8_t DEFAULT_PX = 6;

/**
 * Draw a QR code on the display.
 * @param renderer  The GfxRenderer to draw on
 * @param x         Top-left X coordinate
 * @param y         Top-left Y coordinate
 * @param data      The data to encode in the QR code
 * @param px        Pixels per QR module (default: 6)
 */
inline void drawQRCode(const GfxRenderer& renderer, const int x, const int y, const std::string& data,
                       const uint8_t px = DEFAULT_PX) {
  QRCode qrcode;
  uint8_t qrcodeBytes[qrcode_getBufferSize(4)];
  qrcode_initText(&qrcode, qrcodeBytes, 4, ECC_LOW, data.c_str());
  for (uint8_t cy = 0; cy < qrcode.size; cy++) {
    for (uint8_t cx = 0; cx < qrcode.size; cx++) {
      if (qrcode_getModule(&qrcode, cx, cy)) {
        renderer.fillRect(x + px * cx, y + px * cy, px, px, true);
      }
    }
  }
}

/**
 * Calculate the total pixel size of a QR code.
 * @param px  Pixels per module
 * @return    Total size in pixels (width = height)
 */
constexpr int qrSize(const uint8_t px = DEFAULT_PX) { return px * QR_MODULES; }

}  // namespace QRCodeHelper

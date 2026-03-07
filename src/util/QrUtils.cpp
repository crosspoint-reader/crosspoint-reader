#include "QrUtils.h"

#include <qrcode.h>

#include <algorithm>
#include <memory>

#include "Logging.h"
#include "fontIds.h"

void QrUtils::drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload) {
  // Dynamically calculate the QR code version based on text length.
  // Capacities are for ECC_LOW byte mode, with overhead for mode indicator and length field.
  const size_t len = textPayload.length();

  if (len > 2953) {
    LOG_ERR("QR", "Text payload (%d bytes) exceeds QR v40 max capacity", (int)len);
    renderer.drawCenteredText(UI_12_FONT_ID, bounds.y + bounds.height / 2, "Text too large for QR code", true);
    return;
  }

  int version = 4;
  if (len > 78) version = 10;
  if (len > 271) version = 20;
  if (len > 858) version = 30;
  if (len > 1732) version = 40;

  // Make sure we have a large enough buffer on the heap to avoid blowing the stack
  uint32_t bufferSize = qrcode_getBufferSize(version);
  auto qrcodeBytes = std::make_unique<uint8_t[]>(bufferSize);

  QRCode qrcode;
  // Initialize the QR code. We use ECC_LOW for max capacity.
  int8_t res = qrcode_initText(&qrcode, qrcodeBytes.get(), version, ECC_LOW, textPayload.c_str());

  if (res == 0) {
    // Determine the optimal pixel size.
    const int maxDim = std::min(bounds.width, bounds.height);

    int px = maxDim / qrcode.size;
    if (px < 1) px = 1;

    // Calculate centering X and Y
    const int qrDisplaySize = qrcode.size * px;
    const int xOff = bounds.x + (bounds.width - qrDisplaySize) / 2;
    const int yOff = bounds.y + (bounds.height - qrDisplaySize) / 2;

    // Draw the QR Code
    for (uint8_t cy = 0; cy < qrcode.size; cy++) {
      for (uint8_t cx = 0; cx < qrcode.size; cx++) {
        if (qrcode_getModule(&qrcode, cx, cy)) {
          renderer.fillRect(xOff + px * cx, yOff + px * cy, px, px, true);
        }
      }
    }
  } else {
    // If it fails (e.g. text too large), log and show error
    LOG_ERR("QR", "QR code generation failed for version %d (%d bytes)", version, (int)len);
    renderer.drawCenteredText(UI_12_FONT_ID, bounds.y + bounds.height / 2, "Text too large for QR code", true);
  }
}

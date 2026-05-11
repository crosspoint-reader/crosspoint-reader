#include "QrUtils.h"

#include <Utf8.h>

#include <algorithm>
#include <memory>

#include "Logging.h"
#include "Memory.h"
#include "fontIds.h"
#include "qrcodegen.h"

void QrUtils::drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload) {
  constexpr uint8_t qrcode_VERSION_MIN = 1;
  constexpr uint8_t qrcode_VERSION_MAX = 20;  // Alphanumeric: 1249 chars, Kanji: 528 chars, Numeric: 2061 chars

  const size_t qrBufLen = qrcodegen_BUFFER_LEN_FOR_VERSION(qrcode_VERSION_MAX);  // 1178 bytes
  auto qrcode = makeUniqueNoThrow<uint8_t[]>(qrBufLen);
  auto tempBuffer = makeUniqueNoThrow<uint8_t[]>(qrBufLen);
  if (!qrcode || !tempBuffer) {
    LOG_ERR("QR", "OOM: %d bytes", (int)qrBufLen);
    return;
  }

  const char* payload = textPayload.c_str();
  const bool res = qrcodegen_encodeText(payload, tempBuffer.get(), qrcode.get(), qrcodegen_Ecc_LOW, qrcode_VERSION_MIN,
                                        qrcode_VERSION_MAX, qrcodegen_Mask_AUTO, true);

  tempBuffer.reset();

  if (res) {
    // Determine the optimal pixel size.
    const int maxDim = std::min(bounds.width, bounds.height);
    const int qrSize = qrcodegen_getSize(qrcode.get());

    int px = maxDim / qrSize;
    if (px < 1) px = 1;

    // Calculate centering X and Y
    const int qrDisplaySize = qrSize * px;
    const int xOff = bounds.x + (bounds.width - qrDisplaySize) / 2;
    const int yOff = bounds.y + (bounds.height - qrDisplaySize) / 2;

    // Draw the QR Code
    for (uint8_t cy = 0; cy < qrSize; cy++) {
      for (uint8_t cx = 0; cx < qrSize; cx++) {
        if (qrcodegen_getModule(qrcode.get(), cx, cy)) {
          renderer.fillRect(xOff + px * cx, yOff + px * cy, px, px, true);
        }
      }
    }
  } else {
    const size_t len = textPayload.length();
    // If it fails (e.g. text too large), log an error
    std::string errMsg = "Failed to generate a QR Code for payload of length " + std::to_string(len);
    LOG_ERR("QR", "%s", errMsg.c_str());

    constexpr int fontId = UI_12_FONT_ID;
    const int lineHeight = renderer.getLineHeight(fontId);
    const auto lines = renderer.wrappedText(fontId, errMsg.c_str(), bounds.width, 2);

    const int totalHeight = static_cast<int>(lines.size()) * lineHeight;
    int lineY = bounds.y + (bounds.height - totalHeight) / 2;
    for (const auto& line : lines) {
      const int lineWidth = renderer.getTextWidth(fontId, line.c_str());
      const int lineX = bounds.x + (bounds.width - lineWidth) / 2;
      renderer.drawText(fontId, lineX, lineY, line.c_str(), true);
      lineY += lineHeight;
    }
  }
}

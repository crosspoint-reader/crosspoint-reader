#include "QrUtils.h"

#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "Logging.h"
#include "Memory.h"
#include "fontIds.h"
#include "qrcodegen.h"

static bool hasNonAscii(const char* str) {
  for (; *str != '\0'; ++str) {
    if (static_cast<uint8_t>(*str) >= 0x80) {
      LOG_DBG("QR", "Payload contains non-ASCII characters");
      return true;
    }
  }
  LOG_DBG("QR", "Payload is ASCII");
  return false;
}

static void drawCenteredWrappedText(const GfxRenderer& renderer, const Rect& bounds, int fontId, const char* text,
                                    int maxLines) {
  const int lineHeight = renderer.getLineHeight(fontId);
  const auto lines = renderer.wrappedText(fontId, text, bounds.width, maxLines);

  const int totalHeight = static_cast<int>(lines.size()) * lineHeight;
  int lineY = bounds.y + (bounds.height - totalHeight) / 2;
  for (const auto& line : lines) {
    const int lineWidth = renderer.getTextWidth(fontId, line.c_str());
    const int lineX = bounds.x + (bounds.width - lineWidth) / 2;
    renderer.drawText(fontId, lineX, lineY, line.c_str(), true);
    lineY += lineHeight;
  }
}

void QrUtils::drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload) {
  constexpr uint8_t qrcode_VERSION_MIN = 1;
  constexpr uint8_t qrcode_VERSION_MAX = 40;

  const size_t qrBufLen = qrcodegen_BUFFER_LEN_FOR_VERSION(qrcode_VERSION_MAX);  // 3918 bytes
  auto qrcode = makeUniqueNoThrow<uint8_t[]>(qrBufLen);
  auto tempBuffer = makeUniqueNoThrow<uint8_t[]>(qrBufLen);
  if (!qrcode || !tempBuffer) {
    const std::string errMsg = "Failed to allocate QR Code buffer: " + std::to_string(qrBufLen) + " bytes";
    LOG_ERR("QR", "%s", errMsg.c_str());

    constexpr int fontId = UI_12_FONT_ID;
    drawCenteredWrappedText(renderer, bounds, fontId, errMsg.c_str(), 2);
    return;
  }

  const char* payload = textPayload.c_str();
  const size_t textLen = textPayload.length();
  bool res;

  if (hasNonAscii(payload)) {
    // Non-ASCII path: ECI 26 + BYTE mode for spec-compliant UTF-8 encoding.
    // Even though modern scanners often default to UTF-8, ISO/IEC 18004 specifies
    // that BYTE mode without an ECI segment defaults to ISO 8859-1 (Latin-1), not UTF-8.
    // A strictly spec-compliant scanner will misinterpret UTF-8 multi-byte sequences
    // as individual Latin-1 characters.
    uint8_t eciBuf[3] = {0};
    struct qrcodegen_Segment eciSeg = qrcodegen_makeEci(26, eciBuf);

    if (textLen <= qrBufLen) {
      memcpy(tempBuffer.get(), payload, textLen);
      struct qrcodegen_Segment byteSeg;
      byteSeg.mode = qrcodegen_Mode_BYTE;
      byteSeg.numChars = static_cast<int>(textLen);
      byteSeg.data = tempBuffer.get();
      byteSeg.bitLength = static_cast<int>(textLen) * 8;

      struct qrcodegen_Segment segs[2] = {eciSeg, byteSeg};
      res = qrcodegen_encodeSegmentsAdvanced(segs, 2, qrcodegen_Ecc_LOW, qrcode_VERSION_MIN, qrcode_VERSION_MAX,
                                             qrcodegen_Mask_AUTO, true, tempBuffer.get(), qrcode.get());
    } else {
      res = false;
    }
  } else {
    // ASCII path: standard encoding (may use numeric/alphanumeric/byte mode optimally)
    res = qrcodegen_encodeText(payload, tempBuffer.get(), qrcode.get(), qrcodegen_Ecc_LOW, qrcode_VERSION_MIN,
                               qrcode_VERSION_MAX, qrcodegen_Mask_AUTO, true);
  }

  tempBuffer.reset();

  if (!res) {
    const size_t len = textPayload.length();
    // If it fails (e.g. text too large), log an error
    const std::string errMsg = "Failed to generate a QR Code for payload of length " + std::to_string(len);
    LOG_ERR("QR", "%s", errMsg.c_str());

    constexpr int fontId = UI_12_FONT_ID;
    drawCenteredWrappedText(renderer, bounds, fontId, errMsg.c_str(), 2);
    return;
  }

  // Determine the optimal pixel size.
  const int maxDim = std::min(bounds.width, bounds.height);
  // QR codes require a minimum quiet zone of 4 modules on every side by spec.
  const int qrSize = qrcodegen_getSize(qrcode.get()) + 8;

  int px = maxDim / qrSize;
  if (px < 1) {
    const size_t len = textPayload.length();
    const std::string errMsg = "Invalid QR Code size for payload of length " + std::to_string(len) +
                               ": requires at least " + std::to_string(qrSize) + " pixels, but only " +
                               std::to_string(maxDim) + " available";
    LOG_ERR("QR", "%s", errMsg.c_str());

    constexpr int fontId = UI_12_FONT_ID;
    drawCenteredWrappedText(renderer, bounds, fontId, errMsg.c_str(), 2);
    return;
  }
  // Calculate centering X and Y
  const int qrDisplaySize = qrSize * px;
  const int xOff = bounds.x + (bounds.width - qrDisplaySize) / 2 + 4 * px;
  const int yOff = bounds.y + (bounds.height - qrDisplaySize) / 2 + 4 * px;

  // Draw the QR Code
  for (uint8_t cy = 0; cy < qrSize; cy++) {
    for (uint8_t cx = 0; cx < qrSize; cx++) {
      if (qrcodegen_getModule(qrcode.get(), cx, cy)) {
        renderer.fillRect(xOff + px * cx, yOff + px * cy, px, px, true);
      }
    }
  }
}

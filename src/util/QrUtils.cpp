#include "QrUtils.h"

#include <Utf8.h>
#include <qrcodegen.h>

#include <algorithm>
#include <memory>

#include "Logging.h"

namespace {

bool hasNonAscii(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (data[i] > 0x7F) return true;
  }
  return false;
}

}  // namespace

bool QrUtils::drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload) {
  bool truncated = false;
  size_t len = textPayload.size();
  const char* text = textPayload.c_str();

  // Truncate at a UTF-8 safe boundary if needed
  std::string truncatedStr;
  if (len > MAX_QR_CAPACITY) {
    len = utf8SafeTruncateBuffer(text, static_cast<int>(MAX_QR_CAPACITY));
    truncatedStr = textPayload.substr(0, len);
    text = truncatedStr.c_str();
    truncated = true;
    LOG_DBG("QR", "Truncated payload from %u to %u bytes", textPayload.size(), len);
  }

  // Heap-allocate both buffers (each ~3918 bytes for version 40)
  constexpr size_t bufLen = qrcodegen_BUFFER_LEN_FOR_VERSION(qrcodegen_VERSION_MAX);
  auto qrcode = std::make_unique<uint8_t[]>(bufLen);
  auto tempBuf = std::make_unique<uint8_t[]>(bufLen);

  bool ok = false;
  const auto* rawData = reinterpret_cast<const uint8_t*>(text);

  if (hasNonAscii(rawData, len)) {
    // Non-ASCII content: use ECI mode 26 (UTF-8) + byte segment via the low-level API
    // so scanners know the encoding rather than assuming ISO 8859-1.
    uint8_t eciBuf[4] = {};
    struct qrcodegen_Segment eciSeg = qrcodegen_makeEci(26, eciBuf);

    // Build byte segment — the segment data buffer can overlap with tempBuf
    const size_t segBufSize = qrcodegen_calcSegmentBufferSize(qrcodegen_Mode_BYTE, len);
    auto segBuf = std::make_unique<uint8_t[]>(segBufSize);
    struct qrcodegen_Segment byteSeg = qrcodegen_makeBytes(rawData, len, segBuf.get());

    struct qrcodegen_Segment segs[2] = {eciSeg, byteSeg};
    ok = qrcodegen_encodeSegmentsAdvanced(segs, 2, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                          qrcodegen_Mask_AUTO, false, tempBuf.get(), qrcode.get());
  } else {
    // ASCII-only: let the library auto-select the optimal mode (numeric/alphanumeric/byte)
    ok = qrcodegen_encodeText(text, tempBuf.get(), qrcode.get(), qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                              qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, false);
  }

  if (ok) {
    const int size = qrcodegen_getSize(qrcode.get());
    const int maxDim = std::min(bounds.width, bounds.height);

    int px = maxDim / size;
    if (px < 1) px = 1;

    const int qrDisplaySize = size * px;
    const int xOff = bounds.x + (bounds.width - qrDisplaySize) / 2;
    const int yOff = bounds.y + (bounds.height - qrDisplaySize) / 2;

    for (int cy = 0; cy < size; cy++) {
      for (int cx = 0; cx < size; cx++) {
        if (qrcodegen_getModule(qrcode.get(), cx, cy)) {
          renderer.fillRect(xOff + px * cx, yOff + px * cy, px, px, true);
        }
      }
    }
  } else {
    LOG_ERR("QR", "Failed to encode QR code (%u bytes)", len);
  }

  return truncated;
}

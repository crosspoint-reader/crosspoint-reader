#include "QrDisplayActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <qrcode.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void QrDisplayActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void QrDisplayActivity::onExit() { Activity::onExit(); }

void QrDisplayActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    onGoBack();
    return;
  }
}

void QrDisplayActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DISPLAY_QR), nullptr);

  // Dynamically calculate the QR code version based on text length
  // Version 4 holds ~114 bytes, Version 10 ~395, Version 20 ~1066, up to 40
  // qrcode.h max version is 40.
  // Formula: approx version = size / 26 + 1 (very rough estimate, better to find best fit)
  const size_t len = textPayload.length();
  int version = 4;
  if (len > 114) version = 10;
  if (len > 395) version = 20;
  if (len > 1066) version = 30;
  if (len > 2110) version = 40;

  // Make sure we have a large enough buffer on the heap to avoid blowing the stack
  uint32_t bufferSize = qrcode_getBufferSize(version);
  uint8_t* qrcodeBytes = new uint8_t[bufferSize];

  QRCode qrcode;
  // Initialize the QR code. We use ECC_LOW for max capacity.
  int8_t res = qrcode_initText(&qrcode, qrcodeBytes, version, ECC_LOW, textPayload.c_str());

  if (res == 0) {
    // Determine the optimal pixel size.
    // Leave some margin (e.g., 40 pixels total)
    const int availableWidth = pageWidth - 40;
    const int availableHeight = pageHeight - metrics.topPadding - metrics.headerHeight - metrics.verticalSpacing * 2 - 40;
    const int maxDim = std::min(availableWidth, availableHeight);

    int px = maxDim / qrcode.size;
    if (px < 1) px = 1;

    // Calculate centering X and Y
    const int qrDisplaySize = qrcode.size * px;
    const int xOff = (pageWidth - qrDisplaySize) / 2;
    const int startY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int yOff = startY + (availableHeight - qrDisplaySize) / 2;

    // Draw the QR Code
    for (uint8_t cy = 0; cy < qrcode.size; cy++) {
      for (uint8_t cx = 0; cx < qrcode.size; cx++) {
        if (qrcode_getModule(&qrcode, cx, cy)) {
          renderer.fillRect(xOff + px * cx, yOff + px * cy, px, px, true);
        }
      }
    }
  } else {
    // If it fails (e.g. text too large), show an error message
    std::string errMsg = "Text too large for QR Code";
    renderer.drawText(UI_10_FONT_ID, 20, pageHeight / 2, errMsg.c_str());
  }

  delete[] qrcodeBytes;

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

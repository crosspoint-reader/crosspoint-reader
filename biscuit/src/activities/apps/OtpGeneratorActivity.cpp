#include "OtpGeneratorActivity.h"

#include <GfxRenderer.h>
#include <esp_random.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void OtpGeneratorActivity::generatePage() {
  esp_fill_random(pageData, sizeof(pageData));
}

void OtpGeneratorActivity::onEnter() {
  Activity::onEnter();
  pageNumber = 0;
  generatePage();
  requestUpdate();
}

void OtpGeneratorActivity::onExit() { Activity::onExit(); }

void OtpGeneratorActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) { finish(); return; }

  if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
      mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    if (pageNumber > 0) { pageNumber--; generatePage(); requestUpdate(); }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
      mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    pageNumber++;
    generatePage();
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    // Regenerate current page
    generatePage();
    requestUpdate();
  }
}

void OtpGeneratorActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  char title[24];
  snprintf(title, sizeof(title), "OTP Pad - Page %d", pageNumber + 1);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title);

  const int startY = metrics.topPadding + metrics.headerHeight + 16;
  const int cellW = (pageWidth - 32) / COLS;
  const int cellH = renderer.getLineHeight(SMALL_FONT_ID) + 6;

  for (int row = 0; row < ROWS; row++) {
    for (int col = 0; col < COLS; col++) {
      int idx = row * COLS + col;
      char hex[4];
      snprintf(hex, sizeof(hex), "%02X", pageData[idx]);
      int x = 16 + col * cellW;
      int y = startY + row * cellH;
      renderer.drawText(SMALL_FONT_ID, x, y, hex);
    }
  }

  const auto labels = mappedInput.mapLabels("Back", "New", "Prev", "Next");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

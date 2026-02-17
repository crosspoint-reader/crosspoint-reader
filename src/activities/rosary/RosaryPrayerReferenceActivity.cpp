#include "RosaryPrayerReferenceActivity.h"

#include <GfxRenderer.h>

#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void RosaryPrayerReferenceActivity::taskTrampoline(void* param) {
  auto* self = static_cast<RosaryPrayerReferenceActivity*>(param);
  self->displayTaskLoop();
}

void RosaryPrayerReferenceActivity::onEnter() {
  Activity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  selectorIndex = 0;
  showingPrayerText = false;
  updateRequired = true;

  xTaskCreate(&RosaryPrayerReferenceActivity::taskTrampoline, "PrayerRefTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void RosaryPrayerReferenceActivity::onExit() {
  Activity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void RosaryPrayerReferenceActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (showingPrayerText) {
      showingPrayerText = false;
      updateRequired = true;
    } else {
      onComplete();
    }
    return;
  }

  if (showingPrayerText) {
    // In prayer text view, no navigation needed (text fits on screen)
    // Back button already handled above
    return;
  }

  buttonNavigator.onNext([this] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, RosaryData::PRAYER_REFERENCE_COUNT);
    updateRequired = true;
  });

  buttonNavigator.onPrevious([this] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, RosaryData::PRAYER_REFERENCE_COUNT);
    updateRequired = true;
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    selectedPrayer = selectorIndex;
    showingPrayerText = true;
    updateRequired = true;
  }
}

void RosaryPrayerReferenceActivity::drawWrappedText(int fontId, int x, int y, int maxWidth, int maxHeight,
                                                     const char* text, EpdFontFamily::Style style) const {
  if (!text || text[0] == '\0') return;

  const int lineHeight = renderer.getLineHeight(fontId);
  const int spaceWidth = renderer.getSpaceWidth(fontId);
  int currentX = x;
  int currentY = y;

  const char* pos = text;

  while (*pos != '\0') {
    const char* wordEnd = pos;
    while (*wordEnd != '\0' && *wordEnd != ' ' && *wordEnd != '\n') {
      wordEnd++;
    }

    int wordLen = wordEnd - pos;
    if (wordLen > 0) {
      char wordBuf[128];
      int copyLen = wordLen < 127 ? wordLen : 127;
      memcpy(wordBuf, pos, copyLen);
      wordBuf[copyLen] = '\0';

      int wordWidth = renderer.getTextWidth(fontId, wordBuf, style);

      if (currentX > x && (currentX + wordWidth) > (x + maxWidth)) {
        currentX = x;
        currentY += lineHeight;
        if (currentY + lineHeight > y + maxHeight) {
          return;
        }
      }

      renderer.drawText(fontId, currentX, currentY, wordBuf, true, style);
      currentX += wordWidth;
    }

    if (*wordEnd == ' ') {
      currentX += spaceWidth;
      pos = wordEnd + 1;
    } else if (*wordEnd == '\n') {
      currentX = x;
      currentY += lineHeight;
      if (currentY + lineHeight > y + maxHeight) {
        return;
      }
      pos = wordEnd + 1;
    } else {
      pos = wordEnd;
    }
  }
}

void RosaryPrayerReferenceActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void RosaryPrayerReferenceActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();
  const int sidePadding = metrics.contentSidePadding;

  if (showingPrayerText) {
    // Show the selected prayer's full text
    const char* prayerName = RosaryData::getPrayerReferenceName(selectedPrayer);
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, prayerName);

    int contentY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 4;

    // Draw prayer title
    renderer.drawCenteredText(UI_12_FONT_ID, contentY, prayerName, true, EpdFontFamily::BOLD);
    contentY += renderer.getLineHeight(UI_12_FONT_ID) + 8;

    // Separator
    renderer.drawLine(sidePadding, contentY, pageWidth - sidePadding, contentY);
    contentY += 10;

    // Draw prayer text
    const char* prayerText = RosaryData::getPrayerReferenceText(selectedPrayer);
    int textAreaHeight = pageHeight - contentY - metrics.buttonHintsHeight - metrics.verticalSpacing;
    drawWrappedText(UI_10_FONT_ID, sidePadding, contentY, pageWidth - sidePadding * 2, textAreaHeight, prayerText);

    const auto labels = mappedInput.mapLabels("\x11 Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    // Show prayer list
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Rosary Prayers");

    int contentY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    int contentHeight = pageHeight - contentY - metrics.buttonHintsHeight - metrics.verticalSpacing;

    GUI.drawList(
        renderer, Rect{0, contentY, pageWidth, contentHeight}, RosaryData::PRAYER_REFERENCE_COUNT, selectorIndex,
        [](int index) { return std::string(RosaryData::getPrayerReferenceName(index)); }, nullptr, nullptr, nullptr);

    const auto labels = mappedInput.mapLabels("\x11 Back", "View", "Up", "Down");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

#include "XtcReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "fontIds.h"

namespace {
constexpr int RAPID_NAV_START_MS = 500;
constexpr int RAPID_NAV_DELAY_MS = 700;
}  // namespace

int XtcReaderChapterSelectionActivity::getPageItems() const {
  constexpr int startY = 60;
  constexpr int lineHeight = 30;

  const int screenHeight = renderer.getScreenHeight();
  const int endY = screenHeight - lineHeight;

  const int availableHeight = endY - startY;
  int items = availableHeight / lineHeight;
  if (items < 1) {
    items = 1;
  }
  return items;
}

int XtcReaderChapterSelectionActivity::findChapterIndexForPage(uint32_t page) const {
  if (!xtc) {
    return 0;
  }

  const auto& chapters = xtc->getChapters();
  for (size_t i = 0; i < chapters.size(); i++) {
    if (page >= chapters[i].startPage && page <= chapters[i].endPage) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

void XtcReaderChapterSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<XtcReaderChapterSelectionActivity*>(param);
  self->displayTaskLoop();
}

void XtcReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();

  if (!xtc) {
    return;
  }

  renderingMutex = xSemaphoreCreateMutex();
  selectorIndex = findChapterIndexForPage(currentPage);

  updateRequired = true;
  xTaskCreate(&XtcReaderChapterSelectionActivity::taskTrampoline, "XtcReaderChapterSelectionActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void XtcReaderChapterSelectionActivity::onExit() {
  Activity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void XtcReaderChapterSelectionActivity::loop() {
  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool prevPressed =
      mappedInput.isPressed(MappedInputManager::Button::Up) || mappedInput.isPressed(MappedInputManager::Button::Left);
  const bool nextPressed = mappedInput.isPressed(MappedInputManager::Button::Down) ||
                           mappedInput.isPressed(MappedInputManager::Button::Right);

  const int pageItems = getPageItems();
  const int currentTocPage = selectorIndex / pageItems;
  const int total = static_cast<int>(xtc->getChapters().size());
  const int lastTocPage = total > 0 ? (total - 1) / pageItems : 0;

  const bool shouldNavigateRapidly = mappedInput.getHeldTime() > RAPID_NAV_START_MS;
  const bool isRapidNavigationDue = (millis() - lastRapidNavTime) > RAPID_NAV_DELAY_MS;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto& chapters = xtc->getChapters();
    if (!chapters.empty() && selectorIndex >= 0 && selectorIndex < static_cast<int>(chapters.size())) {
      onSelectPage(chapters[selectorIndex].startPage);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } else if (prevReleased) {
    if (total == 0 || lastRapidNavTime != 0) {
      lastRapidNavTime = 0;
      return;
    }

    selectorIndex = (selectorIndex + total - 1) % total;
    updateRequired = true;
  } else if (nextReleased) {
    if (total == 0 || lastRapidNavTime != 0) {
      lastRapidNavTime = 0;
      return;
    }

    selectorIndex = (selectorIndex + 1) % total;
    updateRequired = true;
  } else if (prevPressed && shouldNavigateRapidly && isRapidNavigationDue) {
    if (total == 0) {
      return;
    }

    selectorIndex = currentTocPage > 0 ? (currentTocPage - 1) * pageItems : lastTocPage * pageItems;
    updateRequired = true;
    lastRapidNavTime = millis();
  } else if (nextPressed && shouldNavigateRapidly && isRapidNavigationDue) {
    if (total == 0) {
      return;
    }

    selectorIndex = currentTocPage < lastTocPage ? (currentTocPage + 1) * pageItems : 0;
    updateRequired = true;
    lastRapidNavTime = millis();
  }
}

void XtcReaderChapterSelectionActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void XtcReaderChapterSelectionActivity::renderScreen() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const int pageItems = getPageItems();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Select Chapter", true, EpdFontFamily::BOLD);

  const auto& chapters = xtc->getChapters();
  if (chapters.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, 120, "No chapters");
    renderer.displayBuffer();
    return;
  }

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  renderer.fillRect(0, 60 + (selectorIndex % pageItems) * 30 - 2, pageWidth - 1, 30);
  for (int i = pageStartIndex; i < static_cast<int>(chapters.size()) && i < pageStartIndex + pageItems; i++) {
    const auto& chapter = chapters[i];
    const char* title = chapter.name.empty() ? "Unnamed" : chapter.name.c_str();
    renderer.drawText(UI_10_FONT_ID, 20, 60 + (i % pageItems) * 30, title, i != selectorIndex);
  }

  const auto labels = mappedInput.mapLabels("« Back", "Select", "Up", "Down");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

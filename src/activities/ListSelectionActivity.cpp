#include "ListSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "fontIds.h"

void ListSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<ListSelectionActivity*>(param);
  self->displayTaskLoop();
}

int ListSelectionActivity::getPageItems() const {
  const int screenHeight = renderer.getScreenHeight();
  const int availableHeight = screenHeight - START_Y - BOTTOM_BAR_HEIGHT;
  const int pageItems = (availableHeight / LINE_HEIGHT);
  return pageItems > 0 ? pageItems : 1;
}

void ListSelectionActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  enterTime = millis();

  // Load items (allows subclasses to populate data)
  loadItems();

  // Ensure selector index is valid
  const size_t itemCount = getItemCount();
  if (selectorIndex >= itemCount && itemCount > 0) {
    selectorIndex = 0;
  }

  updateRequired = true;

  xTaskCreate(&ListSelectionActivity::taskTrampoline, "ListSelectionTask", 2048, this, 1, &displayTaskHandle);
}

void ListSelectionActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void ListSelectionActivity::loop() {
  const unsigned long timeSinceEnter = millis() - enterTime;
  if (timeSinceEnter < IGNORE_INPUT_MS) {
    return;
  }

  const size_t itemCount = getItemCount();
  if (itemCount == 0) {
    // Handle back button even when empty
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onBack();
    }
    return;
  }

  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = getPageItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex < itemCount) {
      onItemSelected(selectorIndex);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBack();
  } else if (prevReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems - 1) * pageItems + itemCount) % itemCount;
    } else {
      selectorIndex = (selectorIndex + itemCount - 1) % itemCount;
    }
    updateRequired = true;
  } else if (nextReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems + 1) * pageItems) % itemCount;
    } else {
      selectorIndex = (selectorIndex + 1) % itemCount;
    }
    updateRequired = true;
  }
}

void ListSelectionActivity::displayTaskLoop() {
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

void ListSelectionActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, title.c_str(), true, EpdFontFamily::BOLD);

  // Help text
  const auto labels = mappedInput.mapLabels(backLabel.c_str(), confirmLabel.c_str(), "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const size_t itemCount = getItemCount();
  if (itemCount == 0) {
    renderer.drawText(UI_10_FONT_ID, 20, START_Y, emptyMessage.c_str());
    renderer.displayBuffer();
    return;
  }

  // Calculate items per page based on screen height
  const int screenHeight = renderer.getScreenHeight();
  const int availableHeight = screenHeight - START_Y - BOTTOM_BAR_HEIGHT;
  const int pageItems = (availableHeight / LINE_HEIGHT);

  // Calculate page start index
  const auto pageStartIndex = selectorIndex / pageItems * pageItems;

  // Draw selection highlight
  const int visibleSelectedIndex = static_cast<int>(selectorIndex - pageStartIndex);
  if (visibleSelectedIndex >= 0 && visibleSelectedIndex < pageItems && selectorIndex < itemCount) {
    renderer.fillRect(0, START_Y + visibleSelectedIndex * LINE_HEIGHT - 2, pageWidth - 1, LINE_HEIGHT);
  }

  // Draw visible items
  int visibleIndex = 0;
  for (size_t i = pageStartIndex; i < itemCount && visibleIndex < pageItems; i++) {
    const bool isSelected = (i == selectorIndex);
    const int itemY = START_Y + visibleIndex * LINE_HEIGHT;

    const std::string itemText = getItemText(i);
    auto truncated = renderer.truncatedText(UI_10_FONT_ID, itemText.c_str(), pageWidth - 40);
    renderer.drawText(UI_10_FONT_ID, 20, itemY, truncated.c_str(), !isSelected);
    visibleIndex++;
  }

  renderer.displayBuffer();
}

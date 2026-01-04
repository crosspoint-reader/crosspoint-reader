#include "ProtocolSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEM_COUNT = 2;
const char* MENU_ITEMS[MENU_ITEM_COUNT] = {"HTTP (Web Browser)", "FTP (File Client)"};
const char* MENU_DESCRIPTIONS[MENU_ITEM_COUNT] = {"Upload/download via web browser",
                                                  "Upload/download via FTP client"};
}  // namespace

void ProtocolSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<ProtocolSelectionActivity*>(param);
  self->displayTaskLoop();
}

void ProtocolSelectionActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Reset selection
  selectedIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&ProtocolSelectionActivity::taskTrampoline, "ProtocolSelectTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void ProtocolSelectionActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void ProtocolSelectionActivity::loop() {
  // Handle back button - cancel
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onCancel();
    return;
  }

  // Handle confirm button - select current option
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    const FileTransferProtocol protocol = (selectedIndex == 0) ? FileTransferProtocol::HTTP : FileTransferProtocol::FTP;
    onProtocolSelected(protocol);
    return;
  }

  // Handle navigation
  const bool prevPressed = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Left);
  const bool nextPressed = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Right);

  if (prevPressed) {
    selectedIndex = (selectedIndex + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
    updateRequired = true;
  } else if (nextPressed) {
    selectedIndex = (selectedIndex + 1) % MENU_ITEM_COUNT;
    updateRequired = true;
  }
}

void ProtocolSelectionActivity::displayTaskLoop() {
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

void ProtocolSelectionActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "File Transfer", true, BOLD);

  // Draw subtitle
  renderer.drawCenteredText(UI_10_FONT_ID, 50, "Select transfer protocol:");

  // Draw menu items centered on screen
  constexpr int itemHeight = 50;  // Height for each menu item (including description)
  const int startY = (pageHeight - (MENU_ITEM_COUNT * itemHeight)) / 2 + 10;

  for (int i = 0; i < MENU_ITEM_COUNT; i++) {
    const int itemY = startY + i * itemHeight;
    const bool isSelected = (i == selectedIndex);

    // Draw selection highlight (black fill) for selected item
    if (isSelected) {
      renderer.fillRect(20, itemY - 2, pageWidth - 40, itemHeight - 6);
    }

    // Draw text: black=false (white text) when selected (on black background)
    //            black=true (black text) when not selected (on white background)
    renderer.drawText(UI_10_FONT_ID, 30, itemY, MENU_ITEMS[i], /*black=*/!isSelected);
    renderer.drawText(SMALL_FONT_ID, 30, itemY + 22, MENU_DESCRIPTIONS[i], /*black=*/!isSelected);
  }

  // Draw help text at bottom
  const auto labels = mappedInput.mapLabels("« Back", "Select", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

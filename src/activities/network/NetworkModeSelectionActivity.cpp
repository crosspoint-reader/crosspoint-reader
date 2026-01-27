#include "NetworkModeSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEM_COUNT = 3;
const char* MENU_ITEMS[MENU_ITEM_COUNT] = {"Join a Network", "Connect to Calibre", "Create Hotspot"};
const char* MENU_DESCRIPTIONS[MENU_ITEM_COUNT] = {
    "Connect to an existing WiFi network",
    "Use Calibre wireless device transfers",
    "Create a WiFi network others can join",
};
}  // namespace

void NetworkModeSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<NetworkModeSelectionActivity*>(param);
  self->displayTaskLoop();
}

void NetworkModeSelectionActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Reset selection
  selectedIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&NetworkModeSelectionActivity::taskTrampoline, "NetworkModeTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void NetworkModeSelectionActivity::onExit() {
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

void NetworkModeSelectionActivity::loop() {
  // Handle back button - cancel
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onCancel();
    return;
  }

  // Handle confirm button - select current option
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    NetworkMode mode = NetworkMode::JOIN_NETWORK;
    if (selectedIndex == 1) {
      mode = NetworkMode::CONNECT_CALIBRE;
    } else if (selectedIndex == 2) {
      mode = NetworkMode::CREATE_HOTSPOT;
    }
    onModeSelected(mode);
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

void NetworkModeSelectionActivity::displayTaskLoop() {
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

void NetworkModeSelectionActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "File Transfer", true, EpdFontFamily::BOLD);

  // Draw subtitle
  renderer.drawCenteredText(UI_10_FONT_ID, 50, "How would you like to connect?");

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

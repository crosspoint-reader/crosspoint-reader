#include "HomeActivity.h"

#include <GfxRenderer.h>
#include <InputManager.h>
#include <SD.h>

#include "CrossPointState.h"
#include "config.h"

void HomeActivity::taskTrampoline(void* param) {
  auto* self = static_cast<HomeActivity*>(param);
  self->displayTaskLoop();
}

int HomeActivity::getMenuItemCount() const { return hasContinueReading ? 4 : 3; }

void HomeActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Check if we have a book to continue reading
  hasContinueReading = !APP_STATE.openEpubPath.empty() && SD.exists(APP_STATE.openEpubPath.c_str());

  selectorIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&HomeActivity::taskTrampoline, "HomeActivityTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void HomeActivity::onExit() {
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

void HomeActivity::loop() {
  const bool prevPressed =
      inputManager.wasPressed(InputManager::BTN_UP) || inputManager.wasPressed(InputManager::BTN_LEFT);
  const bool nextPressed =
      inputManager.wasPressed(InputManager::BTN_DOWN) || inputManager.wasPressed(InputManager::BTN_RIGHT);

  const int menuCount = getMenuItemCount();

  if (inputManager.wasPressed(InputManager::BTN_CONFIRM)) {
    if (hasContinueReading) {
      // Menu: Continue Reading, Browse, File transfer, Settings
      if (selectorIndex == 0) {
        onContinueReading();
      } else if (selectorIndex == 1) {
        onReaderOpen();
      } else if (selectorIndex == 2) {
        onFileTransferOpen();
      } else if (selectorIndex == 3) {
        onSettingsOpen();
      }
    } else {
      // Menu: Browse, File transfer, Settings
      if (selectorIndex == 0) {
        onReaderOpen();
      } else if (selectorIndex == 1) {
        onFileTransferOpen();
      } else if (selectorIndex == 2) {
        onSettingsOpen();
      }
    }
  } else if (prevPressed) {
    selectorIndex = (selectorIndex + menuCount - 1) % menuCount;
    updateRequired = true;
  } else if (nextPressed) {
    selectorIndex = (selectorIndex + 1) % menuCount;
    updateRequired = true;
  }
}

void HomeActivity::displayTaskLoop() {
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

void HomeActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  renderer.drawCenteredText(READER_FONT_ID, 10, "CrossPoint Reader", true, BOLD);

  // Draw selection
  renderer.fillRect(0, 60 + selectorIndex * 30 - 2, pageWidth - 1, 30);

  int menuY = 60;
  int menuIndex = 0;

  if (hasContinueReading) {
    // Extract filename from path for display
    std::string bookName = APP_STATE.openEpubPath;
    const size_t lastSlash = bookName.find_last_of('/');
    if (lastSlash != std::string::npos) {
      bookName = bookName.substr(lastSlash + 1);
    }
    // Remove .epub extension
    if (bookName.length() > 5 && bookName.substr(bookName.length() - 5) == ".epub") {
      bookName.resize(bookName.length() - 5);
    }
    // Truncate if too long
    if (bookName.length() > 25) {
      bookName.resize(22);
      bookName += "...";
    }
    std::string continueLabel = "Continue: " + bookName;
    renderer.drawText(UI_FONT_ID, 20, menuY, continueLabel.c_str(), selectorIndex != menuIndex);
    menuY += 30;
    menuIndex++;
  }

  renderer.drawText(UI_FONT_ID, 20, menuY, "Browse", selectorIndex != menuIndex);
  menuY += 30;
  menuIndex++;

  renderer.drawText(UI_FONT_ID, 20, menuY, "File transfer", selectorIndex != menuIndex);
  menuY += 30;
  menuIndex++;

  renderer.drawText(UI_FONT_ID, 20, menuY, "Settings", selectorIndex != menuIndex);

  renderer.drawButtonHints(UI_FONT_ID, "Back", "Confirm", "Left", "Right");

  renderer.displayBuffer();
}

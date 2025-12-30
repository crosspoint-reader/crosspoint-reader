#include "HomeActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "fontIds.h"

void HomeActivity::taskTrampoline(void* param) {
  auto* self = static_cast<HomeActivity*>(param);
  self->displayTaskLoop();
}

int HomeActivity::getMenuItemCount() const { return hasContinueReading ? 4 : 3; }

void HomeActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Check if we have a book to continue reading
  hasContinueReading = !APP_STATE.openEpubPath.empty() && SdMan.exists(APP_STATE.openEpubPath.c_str());

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
  const bool prevPressed = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Left);
  const bool nextPressed = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Right);

  const int menuCount = getMenuItemCount();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
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
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "CrossPoint Reader", true, BOLD);

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
    std::string continueLabel = "Continue: " + bookName;
    int itemWidth = renderer.getTextWidth(UI_10_FONT_ID, continueLabel.c_str());
    while (itemWidth > renderer.getScreenWidth() - 40 && continueLabel.length() > 8) {
      continueLabel.replace(continueLabel.length() - 5, 5, "...");
      itemWidth = renderer.getTextWidth(UI_10_FONT_ID, continueLabel.c_str());
      Serial.printf("[%lu] [HOM] width: %lu, pageWidth: %lu\n", millis(), itemWidth, pageWidth);
    }

    renderer.drawText(UI_10_FONT_ID, 20, menuY, continueLabel.c_str(), selectorIndex != menuIndex);
    menuY += 30;
    menuIndex++;
  }

  renderer.drawText(UI_10_FONT_ID, 20, menuY, "Browse", selectorIndex != menuIndex);
  menuY += 30;
  menuIndex++;

  renderer.drawText(UI_10_FONT_ID, 20, menuY, "File transfer", selectorIndex != menuIndex);
  menuY += 30;
  menuIndex++;

  renderer.drawText(UI_10_FONT_ID, 20, menuY, "Settings", selectorIndex != menuIndex);

  const auto labels = mappedInput.mapLabels("Back", "Confirm", "Left", "Right");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

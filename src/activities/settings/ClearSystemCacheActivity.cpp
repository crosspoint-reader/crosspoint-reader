#include "ClearSystemCacheActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ClearSystemCacheActivity::taskTrampoline(void* param) {
  auto* self = static_cast<ClearSystemCacheActivity*>(param);
  self->displayTaskLoop();
}

void ClearSystemCacheActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  state = WARNING;
  updateRequired = true;

  xTaskCreate(&ClearSystemCacheActivity::taskTrampoline, "ClearSystemCacheActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void ClearSystemCacheActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void ClearSystemCacheActivity::displayTaskLoop() {
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

void ClearSystemCacheActivity::render() {
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Clear System Cache", true, EpdFontFamily::BOLD);

  if (state == WARNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 60, "This will clear all system cache data.", true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, "Web assets will be re-downloaded", true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, "on next web server start.", true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 20, "Settings and preferences are preserved.", true);

    const auto labels = mappedInput.mapLabels("« Cancel", "Clear", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == CLEARING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Clearing system cache...", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, "System Cache Cleared", true, EpdFontFamily::BOLD);
    String resultText = String(clearedCount) + " items removed";
    if (failedCount > 0) {
      resultText += ", " + String(failedCount) + " failed";
    }
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, resultText.c_str());

    const auto labels = mappedInput.mapLabels("« Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, "Failed to clear system cache", true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, "Check serial output for details");

    const auto labels = mappedInput.mapLabels("« Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void ClearSystemCacheActivity::clearSystemCache() {
  LOG_DBG("CLEAR_SYSTEM_CACHE", "Clearing system cache...");

  clearedCount = 0;
  failedCount = 0;

  // Recursively delete everything in /.crosspoint/data
  if (!recursiveDelete("/.crosspoint/data")) {
    LOG_ERR("CLEAR_SYSTEM_CACHE", "Failed to clear system cache");
    state = FAILED;
    updateRequired = true;
    return;
  }

  LOG_DBG("CLEAR_SYSTEM_CACHE", "System cache cleared: %d removed, %d failed", clearedCount, failedCount);

  state = SUCCESS;
  updateRequired = true;
}

bool ClearSystemCacheActivity::recursiveDelete(const String& path) {
  // Open the directory
  auto dir = Storage.open(path.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }

  char name[128];
  bool success = true;

  // Iterate through all entries
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    String itemName(name);
    String fullPath = path + "/" + itemName;

    if (file.isDirectory()) {
      // Recursively delete subdirectory
      file.close();
      if (!recursiveDelete(fullPath)) {
        success = false;
        failedCount++;
      }
    } else {
      // Delete file
      file.close();
      if (Storage.remove(fullPath.c_str())) {
        clearedCount++;
      } else {
        LOG_ERR("CLEAR_SYSTEM_CACHE", "Failed to remove file: %s", fullPath.c_str());
        success = false;
        failedCount++;
      }
    }
  }
  dir.close();

  // Remove the directory itself (if not the root)
  if (path != "/.crosspoint/data") {
    if (Storage.rmdir(path.c_str())) {
      clearedCount++;
    } else {
      LOG_ERR("CLEAR_SYSTEM_CACHE", "Failed to remove directory: %s", path.c_str());
      success = false;
      failedCount++;
    }
  }

  return success;
}

void ClearSystemCacheActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_DBG("CLEAR_SYSTEM_CACHE", "User confirmed, starting system cache clear");
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      state = CLEARING;
      xSemaphoreGive(renderingMutex);
      updateRequired = true;
      vTaskDelay(10 / portTICK_PERIOD_MS);

      clearSystemCache();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      LOG_DBG("CLEAR_SYSTEM_CACHE", "User cancelled");
      goBack();
    }
    return;
  }

  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }
}
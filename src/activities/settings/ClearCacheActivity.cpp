#include "ClearCacheActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <I18n.h>
#include <SDCardManager.h>

#include "MappedInputManager.h"
#include "fontIds.h"

void ClearCacheActivity::taskTrampoline(void* param) {
  auto* self = static_cast<ClearCacheActivity*>(param);
  self->displayTaskLoop();
}

void ClearCacheActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  state = WARNING;
  updateRequired = false;  // Don't trigger render immediately to avoid race with parent activity

  xTaskCreate(&ClearCacheActivity::taskTrampoline, "ClearCacheActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void ClearCacheActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Set exit flag to prevent clearCache from accessing mutex after deletion
  isExiting = true;

  // Wait for clearCache task to complete (max 10 seconds)
  if (clearCacheTaskHandle) {
    for (int i = 0; i < 1000 && clearCacheTaskHandle != nullptr; i++) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    // Force delete if still running (shouldn't happen)
    if (clearCacheTaskHandle) {
      vTaskDelete(clearCacheTaskHandle);
      clearCacheTaskHandle = nullptr;
    }
  }

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void ClearCacheActivity::displayTaskLoop() {
  // Wait for parent activity's rendering to complete (screen refresh takes ~422ms)
  // Wait 500ms to be safe and avoid race conditions with parent activity
  vTaskDelay(500 / portTICK_PERIOD_MS);
  updateRequired = true;

  while (true) {
    // CRITICAL: Check both updateRequired AND subActivity atomically
    // This prevents race condition where parent and child render simultaneously
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void ClearCacheActivity::render() {
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, TR(CLEAR_READING_CACHE), true, EpdFontFamily::BOLD);

  if (state == WARNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 60, TR(CLEAR_CACHE_WARNING_1), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, TR(CLEAR_CACHE_WARNING_2), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, TR(CLEAR_CACHE_WARNING_3), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, TR(CLEAR_CACHE_WARNING_4), true);

    const auto labels = mappedInput.mapLabels(TR(CANCEL), TR(CONFIRM), "", "");
    renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == CLEARING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, TR(CLEARING_CACHE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, TR(CACHE_CLEARED), true, EpdFontFamily::BOLD);
    String resultText = String(clearedCount) + " " + TR(ITEMS_REMOVED);
    if (failedCount > 0) {
      resultText += ", " + String(failedCount) + " " + TR(FAILED_LOWER);
    }
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, resultText.c_str());

    const auto labels = mappedInput.mapLabels(TR(BACK), "", "", "");
    renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, TR(CLEAR_CACHE_FAILED), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, TR(CHECK_SERIAL_OUTPUT));

    const auto labels = mappedInput.mapLabels(TR(BACK), "", "", "");
    renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void ClearCacheActivity::clearCache() {
  Serial.printf("[%lu] [CLEAR_CACHE] Clearing cache...\n", millis());

  // Check if exiting before starting
  if (isExiting) {
    Serial.printf("[%lu] [CLEAR_CACHE] Aborted: activity is exiting\n", millis());
    clearCacheTaskHandle = nullptr;
    return;
  }

  // Open .crosspoint directory
  auto root = SdMan.open("/.crosspoint");
  if (!root || !root.isDirectory()) {
    Serial.printf("[%lu] [CLEAR_CACHE] Failed to open cache directory\n", millis());
    if (root) root.close();
    if (!isExiting) {
      state = FAILED;
      updateRequired = true;
    }
    clearCacheTaskHandle = nullptr;
    return;
  }

  clearedCount = 0;
  failedCount = 0;
  char name[128];

  // Iterate through all entries in the directory
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    // Check if exiting during iteration
    if (isExiting) {
      file.close();
      root.close();
      Serial.printf("[%lu] [CLEAR_CACHE] Aborted during iteration\n", millis());
      clearCacheTaskHandle = nullptr;
      return;
    }

    file.getName(name, sizeof(name));
    String itemName(name);

    // Only delete directories starting with epub_ or xtc_
    if (file.isDirectory() && (itemName.startsWith("epub_") || itemName.startsWith("xtc_"))) {
      String fullPath = "/.crosspoint/" + itemName;
      Serial.printf("[%lu] [CLEAR_CACHE] Removing cache: %s\n", millis(), fullPath.c_str());

      file.close();  // Close before attempting to delete

      if (SdMan.removeDir(fullPath.c_str())) {
        clearedCount++;
      } else {
        Serial.printf("[%lu] [CLEAR_CACHE] Failed to remove: %s\n", millis(), fullPath.c_str());
        failedCount++;
      }
    } else {
      file.close();
    }
  }
  root.close();

  Serial.printf("[%lu] [CLEAR_CACHE] Cache cleared: %d removed, %d failed\n", millis(), clearedCount, failedCount);

  // Only update state if not exiting
  if (!isExiting) {
    state = SUCCESS;
    updateRequired = true;
  }

  clearCacheTaskHandle = nullptr;
}

void ClearCacheActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      Serial.printf("[%lu] [CLEAR_CACHE] User confirmed, starting cache clear\n", millis());
      state = CLEARING;
      updateRequired = true;

      // Run clearCache in a separate task to avoid blocking loop()
      xTaskCreate(
          [](void* param) {
            auto* self = static_cast<ClearCacheActivity*>(param);
            self->clearCache();
            vTaskDelete(nullptr);
          },
          "ClearCacheTask", 4096, this, 1, &clearCacheTaskHandle);
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      Serial.printf("[%lu] [CLEAR_CACHE] User cancelled\n", millis());
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

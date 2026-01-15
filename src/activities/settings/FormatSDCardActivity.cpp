#include "FormatSDCardActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <esp_system.h>

#include "MappedInputManager.h"
#include "fontIds.h"

void FormatSDCardActivity::taskTrampoline(void* param) {
  auto* self = static_cast<FormatSDCardActivity*>(param);
  self->displayTaskLoop();
}

void FormatSDCardActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  updateRequired = true;

  xTaskCreate(&FormatSDCardActivity::taskTrampoline, "FormatSDCardTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void FormatSDCardActivity::onExit() {
  ActivityWithSubactivity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void FormatSDCardActivity::displayTaskLoop() {
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

void FormatSDCardActivity::render() {
  if (subActivity) return;

  renderer.clearScreen();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Format SD Card", true, EpdFontFamily::BOLD);

  if (state == WAITING_CONFIRMATION) {
    renderer.drawCenteredText(UI_10_FONT_ID, 150, "WARNING!", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 200, "This will ERASE ALL DATA");
    renderer.drawCenteredText(UI_10_FONT_ID, 230, "on the SD card including:");
    renderer.drawCenteredText(UI_10_FONT_ID, 270, "- All books and documents");
    renderer.drawCenteredText(UI_10_FONT_ID, 300, "- Reading progress");
    renderer.drawCenteredText(UI_10_FONT_ID, 330, "- Cached data");
    renderer.drawCenteredText(UI_10_FONT_ID, 380, "This action CANNOT be undone.");

    const auto labels = mappedInput.mapLabels("Cancel", "FORMAT", "", "");
    renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FORMATTING) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, "Formatting...", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 350, "Please wait, do not power off");
  } else if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, 280, "Format Complete!", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 330, "SD card has been formatted.");
    renderer.drawCenteredText(UI_10_FONT_ID, 380, "Device will restart...");
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, "Format Failed", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 350, "Please try again or check SD card.");

    const auto labels = mappedInput.mapLabels("« Back", "", "", "");
    renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

void FormatSDCardActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (state == WAITING_CONFIRMATION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      state = FORMATTING;
      render();  // Render synchronously to show "Formatting..." before we start
      xSemaphoreGive(renderingMutex);
      performFormat();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == SUCCESS) {
    // Auto-restart after brief delay
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    esp_restart();
  }
}

void FormatSDCardActivity::performFormat() {
  Serial.printf("[%lu] [FORMAT] Starting SD card format...\n", millis());

  // Call the format method on SDCardManager
  bool success = SdMan.format(&Serial);

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  state = success ? SUCCESS : FAILED;
  xSemaphoreGive(renderingMutex);
  updateRequired = true;

  Serial.printf("[%lu] [FORMAT] Format %s\n", millis(), success ? "succeeded" : "failed");
}

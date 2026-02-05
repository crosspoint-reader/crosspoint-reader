#include "EpubReaderPercentSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Fine/coarse slider step sizes for percent adjustments.
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 10;
}  // namespace

void EpubReaderPercentSelectionActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  // Set up rendering task and mark first frame dirty.
  renderingMutex = xSemaphoreCreateMutex();
  updateRequired = true;
  xTaskCreate(&EpubReaderPercentSelectionActivity::taskTrampoline, "EpubPercentSlider", 4096, this, 1,
              &displayTaskHandle);
}

void EpubReaderPercentSelectionActivity::onExit() {
  ActivityWithSubactivity::onExit();
  // Ensure the render task is stopped before freeing the mutex.
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void EpubReaderPercentSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderPercentSelectionActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderPercentSelectionActivity::displayTaskLoop() {
  while (true) {
    // Render only when the view is dirty and no subactivity is running.
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void EpubReaderPercentSelectionActivity::adjustPercent(const int delta) {
  // Apply delta and clamp within 0-100.
  percent += delta;
  if (percent < 0) {
    percent = 0;
  } else if (percent > 100) {
    percent = 100;
  }
  updateRequired = true;
}

void EpubReaderPercentSelectionActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  // Back cancels, confirm selects, arrows adjust the percent.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onCancel();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    onSelect(percent);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    adjustPercent(-kSmallStep);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    adjustPercent(kSmallStep);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    adjustPercent(kLargeStep);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    adjustPercent(-kLargeStep);
    return;
  }
}

void EpubReaderPercentSelectionActivity::renderScreen() {
  renderer.clearScreen();

  // Title and numeric percent value.
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Go to Position", true, EpdFontFamily::BOLD);

  const std::string percentText = std::to_string(percent) + "%";
  renderer.drawCenteredText(UI_12_FONT_ID, 90, percentText.c_str(), true, EpdFontFamily::BOLD);

  // Draw slider track.
  const int screenWidth = renderer.getScreenWidth();
  constexpr int barWidth = 360;
  constexpr int barHeight = 16;
  const int barX = (screenWidth - barWidth) / 2;
  const int barY = 140;

  renderer.drawRect(barX, barY, barWidth, barHeight);

  // Fill slider based on percent.
  const int fillWidth = (barWidth - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4);
  }

  // Draw a simple knob centered at the current percent.
  const int knobX = barX + 2 + fillWidth - 2;
  renderer.fillRect(knobX, barY - 4, 4, barHeight + 8, true);

  // Hint text for step sizes.
  renderer.drawCenteredText(SMALL_FONT_ID, barY + 30, "Left/Right: 1%  Up/Down: 10%", true);

  // Button hints follow the current front button layout.
  const auto labels = mappedInput.mapLabels("« Back", "Select", "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

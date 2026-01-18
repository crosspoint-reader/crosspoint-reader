#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/reader/EpubReaderChapterSelectionActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS_COUNT = 2;
const char* menuItems[MENU_ITEMS_COUNT] = {"Chapters", "Settings"};
}  // namespace

void EpubReaderMenuActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderMenuActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderMenuActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!epub) {
    return;
  }

  renderingMutex = xSemaphoreCreateMutex();

  // Trigger first update
  updateRequired = true;
  xTaskCreate(&EpubReaderMenuActivity::taskTrampoline, "EpubReaderMenuActivityTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void EpubReaderMenuActivity::onExit() {
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

void EpubReaderMenuActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedItemIndex == 0) {
      onSelectChapters();
    } else if (selectedItemIndex == 1) {
      onSelectSettings();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } else if (prevReleased) {
    selectedItemIndex = (selectedItemIndex + MENU_ITEMS_COUNT - 1) % MENU_ITEMS_COUNT;
    updateRequired = true;
  } else if (nextReleased) {
    selectedItemIndex = (selectedItemIndex + 1) % MENU_ITEMS_COUNT;
    updateRequired = true;
  }
}

void EpubReaderMenuActivity::onSelectChapters() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  exitActivity();
  enterNewActivity(new EpubReaderChapterSelectionActivity(
      this->renderer, this->mappedInput, epub, currentSpineIndex,
      [this] {
        exitActivity();
        updateRequired = true;
      },
      [this](const int newSpineIndex) {
        exitActivity();
        updateRequired = true;
        onSelectSpineIndex(newSpineIndex);
      }));
  xSemaphoreGive(renderingMutex);
}

void EpubReaderMenuActivity::onSelectSettings() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  exitActivity();
  // sets orientation to portrait when going into settings
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  enterNewActivity(new SettingsActivity(this->renderer, this->mappedInput, [this] {
    // resets orientation based on settings
    switch (SETTINGS.orientation) {
      case CrossPointSettings::ORIENTATION::PORTRAIT:
        renderer.setOrientation(GfxRenderer::Orientation::Portrait);
        break;
      case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
        renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
        break;
      case CrossPointSettings::ORIENTATION::INVERTED:
        renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
        break;
      case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
        renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
        break;
      default:
        break;
    }
    exitActivity();
    resetSectionHelper();
  }));
  xSemaphoreGive(renderingMutex);
}

void EpubReaderMenuActivity::displayTaskLoop() {
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

void EpubReaderMenuActivity::renderScreen() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();

  const std::string title =
      renderer.truncatedText(UI_12_FONT_ID, epub->getTitle().c_str(), pageWidth - 40, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, 15, title.c_str(), true, EpdFontFamily::BOLD);

  renderer.fillRect(0, 60 + (selectedItemIndex % MENU_ITEMS_COUNT) * 30 - 2, pageWidth - 1, 30);
  for (int i = 0; i < MENU_ITEMS_COUNT; i++) {
    renderer.drawText(UI_10_FONT_ID, 35, 60 + (i % MENU_ITEMS_COUNT) * 30, menuItems[i], i != selectedItemIndex);
  }

  const auto labels = mappedInput.mapLabels("« Back", "Select", "Up", "Down");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

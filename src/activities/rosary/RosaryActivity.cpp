#include "RosaryActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "RosaryMysteryListActivity.h"
#include "RosaryPrayerActivity.h"
#include "RosaryPrayerReferenceActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void RosaryActivity::taskTrampoline(void* param) {
  auto* self = static_cast<RosaryActivity*>(param);
  self->displayTaskLoop();
}

void RosaryActivity::onEnter() {
  Activity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  selectorIndex = 0;
  updateRequired = true;

  xTaskCreate(&RosaryActivity::taskTrampoline, "RosaryTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void RosaryActivity::onExit() {
  ActivityWithSubactivity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void RosaryActivity::startRosary(RosaryData::DayOfWeek day) {
  auto onComplete = [this] {
    exitActivity();
    updateRequired = true;
  };

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  exitActivity();
  enterNewActivity(new RosaryPrayerActivity(renderer, mappedInput, day, onComplete));
  xSemaphoreGive(renderingMutex);
}

void RosaryActivity::showMysteryList(RosaryData::DayOfWeek day) {
  auto onComplete = [this] {
    exitActivity();
    updateRequired = true;
  };

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  exitActivity();
  enterNewActivity(new RosaryMysteryListActivity(renderer, mappedInput, day, onComplete));
  xSemaphoreGive(renderingMutex);
}

void RosaryActivity::showPrayerReference() {
  auto onComplete = [this] {
    exitActivity();
    updateRequired = true;
  };

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  exitActivity();
  enterNewActivity(new RosaryPrayerReferenceActivity(renderer, mappedInput, onComplete));
  xSemaphoreGive(renderingMutex);
}

// Menu layout:
// 0-6: Days of the week (Sunday-Saturday)
// 7: View Prayers (reference)
static constexpr int MENU_ITEM_COUNT = 9;

void RosaryActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  buttonNavigator.onNext([this] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, MENU_ITEM_COUNT);
    updateRequired = true;
  });

  buttonNavigator.onPrevious([this] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, MENU_ITEM_COUNT);
    updateRequired = true;
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex < 7) {
      // Start rosary for the selected day
      startRosary(static_cast<RosaryData::DayOfWeek>(selectorIndex));
    } else if (selectorIndex == 7) {
      // Show mystery list for today (default Sunday for reference)
      showMysteryList(RosaryData::DayOfWeek::Sunday);
    } else if (selectorIndex == 8) {
      showPrayerReference();
    }
  }
}

void RosaryActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void RosaryActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Holy Rosary");

  // Build menu items: 7 days + View Mysteries + View Prayers
  auto getLabel = [](int index) -> std::string {
    if (index < 7) {
      auto day = static_cast<RosaryData::DayOfWeek>(index);
      auto mysterySet = RosaryData::getMysterySetForDay(day);
      std::string label = RosaryData::getDayName(day);
      label += " - ";
      label += RosaryData::getMysterySetName(mysterySet);
      return label;
    } else if (index == 7) {
      return "View Mysteries";
    } else {
      return "View Prayers";
    }
  };

  const int contentY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentY - metrics.buttonHintsHeight - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{0, contentY, pageWidth, contentHeight}, MENU_ITEM_COUNT, selectorIndex,
      [&getLabel](int index) { return getLabel(index); }, nullptr, nullptr, nullptr);

  const auto labels = mappedInput.mapLabels("\x11 Back", "Select", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

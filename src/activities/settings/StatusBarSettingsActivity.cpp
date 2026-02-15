#include "StatusBarSettingsActivity.h"

#include <GfxRenderer.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/StatusBar.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 5;
const char* menuNames[MENU_ITEMS] = {"Chapter Page Count", "Book Progress Percentage", "Progress Bar", "Chapter Title",
                                     "Battery"};
}  // namespace

constexpr int PROGRESS_BAR_ITEMS = 3;
const char* progressBarNames[PROGRESS_BAR_ITEMS] = {"Show Book Progress", "Show Chapter Progress", "Hide"};

void StatusBarSettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<StatusBarSettingsActivity*>(param);
  self->displayTaskLoop();
}

void StatusBarSettingsActivity::onEnter() {
  renderingMutex = xSemaphoreCreateMutex();
  selectedIndex = 0;
  updateRequired = true;

  xTaskCreate(&StatusBarSettingsActivity::taskTrampoline, "StatusBarSettingsTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void StatusBarSettingsActivity::onExit() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void StatusBarSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    updateRequired = true;
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    updateRequired = true;
  }
}

void StatusBarSettingsActivity::handleSelection() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (selectedIndex == 0) {
    // Chapter Page Count
    SETTINGS.statusBarChapterPageCount = (SETTINGS.statusBarChapterPageCount + 1) % 2;
  } else if (selectedIndex == 1) {
    // Book Progress %
    SETTINGS.statusBarBookProgressPercentage = (SETTINGS.statusBarBookProgressPercentage + 1) % 2;
  } else if (selectedIndex == 2) {
    // Progress Bar
    SETTINGS.statusBarProgressBar = (SETTINGS.statusBarProgressBar + 1) % 3;
  } else if (selectedIndex == 3) {
    // Chapter Title
    SETTINGS.statusBarChapterTitle = (SETTINGS.statusBarChapterTitle + 1) % 2;
  } else if (selectedIndex == 4) {
    // Show Battery
    SETTINGS.statusBarBattery = (SETTINGS.statusBarBattery + 1) % 2;
  }
  SETTINGS.saveToFile();
  xSemaphoreGive(renderingMutex);
}

void StatusBarSettingsActivity::displayTaskLoop() {
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

void StatusBarSettingsActivity::render() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Customise Status Bar", true, EpdFontFamily::BOLD);

  // Draw selection highlight
  renderer.fillRect(0, 70 + selectedIndex * 30 - 2, pageWidth - 1, 30);

  // Draw menu items
  for (int i = 0; i < MENU_ITEMS; i++) {
    const int settingY = 70 + i * 30;
    const bool isSelected = (i == selectedIndex);

    renderer.drawText(UI_10_FONT_ID, 20, settingY, menuNames[i], !isSelected);

    // Draw status for each setting
    const char* status = "Hide";
    if (i == 0) {
      status = SETTINGS.statusBarChapterPageCount ? "Show" : "Hide";
    } else if (i == 1) {
      status = SETTINGS.statusBarBookProgressPercentage ? "Show" : "Hide";
    } else if (i == 2) {
      status = progressBarNames[SETTINGS.statusBarProgressBar];
    } else if (i == 3) {
      status = SETTINGS.statusBarChapterTitle ? "Show" : "Hide";
    } else if (i == 4) {
      status = SETTINGS.statusBarBattery ? "Show" : "Hide";
    }
    const auto width = renderer.getTextWidth(UI_10_FONT_ID, status);
    renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, settingY, status, !isSelected);
  }

  // Draw button hints
  const auto labels = mappedInput.mapLabels("« Back", "Toggle", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom += SETTINGS.screenMargin;

  int verticalPreviewPadding = 50;

  auto metrics = UITheme::getInstance().getMetrics();

  // Add status bar margin
  const bool showProgressBar = SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE;
  if (SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage || showProgressBar ||
      SETTINGS.statusBarChapterTitle || SETTINGS.statusBarBattery) {
    // Add additional margin for status bar if progress bar is shown
    orientedMarginBottom += 19 - SETTINGS.screenMargin + (showProgressBar ? (metrics.bookProgressBarHeight + 1) : 0);
  }

  StatusBar::renderStatusBar(renderer, orientedMarginRight, orientedMarginBottom, orientedMarginLeft, 75, 8, 32,
                             "Chapter 21", verticalPreviewPadding);

  renderer.drawText(UI_10_FONT_ID, orientedMarginLeft,
                    renderer.getScreenHeight() - orientedMarginBottom - 50 - 19 - 19 - 4, "Preview");

  renderer.displayBuffer();
}

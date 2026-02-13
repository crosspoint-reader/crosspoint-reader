#include "CalibreSettingsActivity.h"

#include <GfxRenderer.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 3;
const char* menuNames[MENU_ITEMS] = {"OPDS Server URL", "Username", "Password"};
}  // namespace

void CalibreSettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<CalibreSettingsActivity*>(param);
  self->displayTaskLoop();
}

void CalibreSettingsActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  selectedIndex = 0;
  updateRequired = true;

  xTaskCreate(&CalibreSettingsActivity::taskTrampoline, "CalibreSettingsTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void CalibreSettingsActivity::onExit() {
  ActivityWithSubactivity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void CalibreSettingsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  // Handle navigation
  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    updateRequired = true;
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    updateRequired = true;
  });
}

void CalibreSettingsActivity::handleSelection() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  if (selectedIndex == 0) {
    // OPDS Server URL
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, "OPDS Server URL", SETTINGS.opdsServerUrl, 10,
        127,    // maxLength
        false,  // not password
        [this](const std::string& url) {
          strncpy(SETTINGS.opdsServerUrl, url.c_str(), sizeof(SETTINGS.opdsServerUrl) - 1);
          SETTINGS.opdsServerUrl[sizeof(SETTINGS.opdsServerUrl) - 1] = '\0';
          SETTINGS.saveToFile();
          exitActivity();
          updateRequired = true;
        },
        [this]() {
          exitActivity();
          updateRequired = true;
        }));
  } else if (selectedIndex == 1) {
    // Username
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, "Username", SETTINGS.opdsUsername, 10,
        63,     // maxLength
        false,  // not password
        [this](const std::string& username) {
          strncpy(SETTINGS.opdsUsername, username.c_str(), sizeof(SETTINGS.opdsUsername) - 1);
          SETTINGS.opdsUsername[sizeof(SETTINGS.opdsUsername) - 1] = '\0';
          SETTINGS.saveToFile();
          exitActivity();
          updateRequired = true;
        },
        [this]() {
          exitActivity();
          updateRequired = true;
        }));
  } else if (selectedIndex == 2) {
    // Password
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, "Password", SETTINGS.opdsPassword, 10,
        63,     // maxLength
        false,  // not password mode
        [this](const std::string& password) {
          strncpy(SETTINGS.opdsPassword, password.c_str(), sizeof(SETTINGS.opdsPassword) - 1);
          SETTINGS.opdsPassword[sizeof(SETTINGS.opdsPassword) - 1] = '\0';
          SETTINGS.saveToFile();
          exitActivity();
          updateRequired = true;
        },
        [this]() {
          exitActivity();
          updateRequired = true;
        }));
  }

  xSemaphoreGive(renderingMutex);
}

void CalibreSettingsActivity::displayTaskLoop() {
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

void CalibreSettingsActivity::render() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "OPDS Browser");
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    "For Calibre, add /opds to your URL");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + metrics.tabBarHeight;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(MENU_ITEMS),
      static_cast<int>(selectedIndex), [this](int index) { return menuNames[index]; }, nullptr, nullptr,
      [this](int index) {
        // Draw status for each setting
        if (index == 0) {
          return (strlen(SETTINGS.opdsServerUrl) > 0) ? SETTINGS.opdsServerUrl : "[Not Set]";
        } else if (index == 1) {
          return (strlen(SETTINGS.opdsUsername) > 0) ? SETTINGS.opdsUsername : "[Not Set]";
        } else if (index == 2) {
          return (strlen(SETTINGS.opdsPassword) > 0) ? "******" : "[Not Set]";
        }
        return "[Not Set]";
      });

  // Draw help text at bottom
  const auto labels = mappedInput.mapLabels("« Back", "Select", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

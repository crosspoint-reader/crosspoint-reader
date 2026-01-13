#include "CalibreSettingsActivity.h"

#include <GfxRenderer.h>
#include <WiFi.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/CalibreWirelessActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 4;
const char* menuNames[MENU_ITEMS] = {"Calibre Web URL", "Username", "Password", "Connect as Wireless Device"};
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

void CalibreSettingsActivity::handleSelection() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  if (selectedIndex == 0) {
    // Calibre Web URL
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, "Calibre Web URL", SETTINGS.opdsServerUrl, 10,
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
        renderer, mappedInput, "Username", SETTINGS.calibreUsername, 10,
        63,     // maxLength
        false,  // not password
        [this](const std::string& username) {
          strncpy(SETTINGS.calibreUsername, username.c_str(), sizeof(SETTINGS.calibreUsername) - 1);
          SETTINGS.calibreUsername[sizeof(SETTINGS.calibreUsername) - 1] = '\0';
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
        renderer, mappedInput, "Password", SETTINGS.calibrePassword, 10,
        63,    // maxLength
        true,  // is password
        [this](const std::string& password) {
          strncpy(SETTINGS.calibrePassword, password.c_str(), sizeof(SETTINGS.calibrePassword) - 1);
          SETTINGS.calibrePassword[sizeof(SETTINGS.calibrePassword) - 1] = '\0';
          SETTINGS.saveToFile();
          exitActivity();
          updateRequired = true;
        },
        [this]() {
          exitActivity();
          updateRequired = true;
        }));
  } else if (selectedIndex == 3) {
    // Wireless Device - launch the activity (handles WiFi connection internally)
    exitActivity();
    if (WiFi.status() != WL_CONNECTED) {
      enterNewActivity(new WifiSelectionActivity(renderer, mappedInput, [this](bool connected) {
        exitActivity();
        if (connected) {
          enterNewActivity(new CalibreWirelessActivity(renderer, mappedInput, [this] {
            exitActivity();
            updateRequired = true;
          }));
        } else {
          updateRequired = true;
        }
      }));
    } else {
      enterNewActivity(new CalibreWirelessActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
    }
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
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Calibre", true, EpdFontFamily::BOLD);

  // Draw selection highlight
  renderer.fillRect(0, 60 + selectedIndex * 30 - 2, pageWidth - 1, 30);

  // Draw menu items
  for (int i = 0; i < MENU_ITEMS; i++) {
    const int settingY = 60 + i * 30;
    const bool isSelected = (i == selectedIndex);

    renderer.drawText(UI_10_FONT_ID, 20, settingY, menuNames[i], !isSelected);

    // Draw status for URL, username, and password settings
    if (i == 0) {
      const char* status = (strlen(SETTINGS.opdsServerUrl) > 0) ? "[Set]" : "[Not Set]";
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, status);
      renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, settingY, status, !isSelected);
    } else if (i == 1) {
      const char* status = (strlen(SETTINGS.calibreUsername) > 0) ? "[Set]" : "[Not Set]";
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, status);
      renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, settingY, status, !isSelected);
    } else if (i == 2) {
      const char* status = (strlen(SETTINGS.calibrePassword) > 0) ? "[Set]" : "[Not Set]";
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, status);
      renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, settingY, status, !isSelected);
    }
  }

  // Draw button hints
  const auto labels = mappedInput.mapLabels("« Back", "Select", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

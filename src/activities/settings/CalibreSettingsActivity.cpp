#include "CalibreSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 3;
const StrId menuNames[MENU_ITEMS] = {StrId::CALIBRE_WEB_URL, StrId::USERNAME, StrId::PASSWORD};
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
    // OPDS Server URL
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, i18n(CALIBRE_WEB_URL), SETTINGS.opdsServerUrl, 10,
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
        renderer, mappedInput, i18n(USERNAME), SETTINGS.opdsUsername, 10,
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
        renderer, mappedInput, i18n(PASSWORD), SETTINGS.opdsPassword, 10,
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
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, i18n(OPDS_BROWSER), true, EpdFontFamily::BOLD);

  // Draw info text about Calibre
  renderer.drawCenteredText(UI_10_FONT_ID, 40, i18n(CALIBRE_URL_HINT));

  // Draw selection highlight
  renderer.fillRect(0, 70 + selectedIndex * 30 - 2, pageWidth - 1, 30);

  // Draw menu items
  for (int i = 0; i < MENU_ITEMS; i++) {
    const int settingY = 70 + i * 30;
    const bool isSelected = (i == selectedIndex);

    renderer.drawText(UI_10_FONT_ID, 20, settingY, I18N.get(menuNames[i]), !isSelected);

    // Draw status for each setting
    std::string status = std::string("[") + i18n(NOT_SET) + "]";
    if (i == 0) {
      status = (strlen(SETTINGS.opdsServerUrl) > 0) ? std::string("[") + i18n(SET) + "]" : std::string("[") + i18n(NOT_SET) + "]";
    } else if (i == 1) {
      status = (strlen(SETTINGS.opdsUsername) > 0) ? std::string("[") + i18n(SET) + "]" : std::string("[") + i18n(NOT_SET) + "]";
    } else if (i == 2) {
      status = (strlen(SETTINGS.opdsPassword) > 0) ? std::string("[") + i18n(SET) + "]" : std::string("[") + i18n(NOT_SET) + "]";
    }
    const auto width = renderer.getTextWidth(UI_10_FONT_ID, status.c_str());
    renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, settingY, status.c_str(), !isSelected);
  }

  // Draw button hints
  const auto labels = mappedInput.mapLabels(i18n(BACK), i18n(SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

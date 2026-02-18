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
const StrId menuNames[MENU_ITEMS] = {StrId::STR_CALIBRE_WEB_URL, StrId::STR_USERNAME, StrId::STR_PASSWORD};
}  // namespace

void CalibreSettingsActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  selectedIndex = 0;
  requestUpdate();
}

void CalibreSettingsActivity::onExit() { ActivityWithSubactivity::onExit(); }

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
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    requestUpdate();
  });
}

void CalibreSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    // OPDS Server URL
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, tr(STR_CALIBRE_WEB_URL), SETTINGS.opdsServerUrl, 10,
        127,    // maxLength
        false,  // not password
        [this](const std::string& url) {
          strncpy(SETTINGS.opdsServerUrl, url.c_str(), sizeof(SETTINGS.opdsServerUrl) - 1);
          SETTINGS.opdsServerUrl[sizeof(SETTINGS.opdsServerUrl) - 1] = '\0';
          SETTINGS.saveToFile();
          exitActivity();
          requestUpdate();
        },
        [this]() {
          exitActivity();
          requestUpdate();
        }));
  } else if (selectedIndex == 1) {
    // Username
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, tr(STR_USERNAME), SETTINGS.opdsUsername, 10,
        63,     // maxLength
        false,  // not password
        [this](const std::string& username) {
          strncpy(SETTINGS.opdsUsername, username.c_str(), sizeof(SETTINGS.opdsUsername) - 1);
          SETTINGS.opdsUsername[sizeof(SETTINGS.opdsUsername) - 1] = '\0';
          SETTINGS.saveToFile();
          exitActivity();
          requestUpdate();
        },
        [this]() {
          exitActivity();
          requestUpdate();
        }));
  } else if (selectedIndex == 2) {
    // Password
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, tr(STR_PASSWORD), SETTINGS.opdsPassword, 10,
        63,     // maxLength
        false,  // not password mode
        [this](const std::string& password) {
          strncpy(SETTINGS.opdsPassword, password.c_str(), sizeof(SETTINGS.opdsPassword) - 1);
          SETTINGS.opdsPassword[sizeof(SETTINGS.opdsPassword) - 1] = '\0';
          SETTINGS.saveToFile();
          exitActivity();
          requestUpdate();
        },
        [this]() {
          exitActivity();
          requestUpdate();
        }));
  }
}

void CalibreSettingsActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_OPDS_BROWSER), true, EpdFontFamily::BOLD);

  // Draw info text about Calibre
  renderer.drawCenteredText(UI_10_FONT_ID, 40, tr(STR_CALIBRE_URL_HINT));

  // Draw selection highlight
  renderer.fillRect(0, 70 + selectedIndex * 30 - 2, pageWidth - 1, 30);

  // Draw menu items
  for (int i = 0; i < MENU_ITEMS; i++) {
    const int settingY = 70 + i * 30;
    const bool isSelected = (i == selectedIndex);

    renderer.drawText(UI_10_FONT_ID, 20, settingY, I18N.get(menuNames[i]), !isSelected);

    // Draw status for each setting
    std::string status = std::string("[") + tr(STR_NOT_SET) + "]";
    if (i == 0) {
      status = (strlen(SETTINGS.opdsServerUrl) > 0) ? std::string("[") + tr(STR_SET) + "]"
                                                    : std::string("[") + tr(STR_NOT_SET) + "]";
    } else if (i == 1) {
      status = (strlen(SETTINGS.opdsUsername) > 0) ? std::string("[") + tr(STR_SET) + "]"
                                                   : std::string("[") + tr(STR_NOT_SET) + "]";
    } else if (i == 2) {
      status = (strlen(SETTINGS.opdsPassword) > 0) ? std::string("[") + tr(STR_SET) + "]"
                                                   : std::string("[") + tr(STR_NOT_SET) + "]";
    }
    const auto width = renderer.getTextWidth(UI_10_FONT_ID, status.c_str());
    renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, settingY, status.c_str(), !isSelected);
  }

  // Draw button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

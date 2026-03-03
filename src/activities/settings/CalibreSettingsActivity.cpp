#include "CalibreSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SettingsList.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 4;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_CALIBRE_WEB_URL, StrId::STR_USERNAME, StrId::STR_PASSWORD,
                                     StrId::STR_OPDS_FILE_FOLDER};
}  // namespace

void CalibreSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  requestUpdate();
}

void CalibreSettingsActivity::onExit() { Activity::onExit(); }

void CalibreSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
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
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_CALIBRE_WEB_URL),
                                                                   SETTINGS.opdsServerUrl, 127, false),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               strncpy(SETTINGS.opdsServerUrl, kb.text.c_str(), sizeof(SETTINGS.opdsServerUrl) - 1);
                               SETTINGS.opdsServerUrl[sizeof(SETTINGS.opdsServerUrl) - 1] = '\0';
                               SETTINGS.saveToFile();
                             }
                           });
  } else if (selectedIndex == 1) {
    // Username
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_USERNAME),
                                                                   SETTINGS.opdsUsername, 63, false),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               strncpy(SETTINGS.opdsUsername, kb.text.c_str(), sizeof(SETTINGS.opdsUsername) - 1);
                               SETTINGS.opdsUsername[sizeof(SETTINGS.opdsUsername) - 1] = '\0';
                               SETTINGS.saveToFile();
                             }
                           });
  } else if (selectedIndex == 2) {
    // Password
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_PASSWORD),
                                                                   SETTINGS.opdsPassword, 63, false),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               strncpy(SETTINGS.opdsPassword, kb.text.c_str(), sizeof(SETTINGS.opdsPassword) - 1);
                               SETTINGS.opdsPassword[sizeof(SETTINGS.opdsPassword) - 1] = '\0';
                               SETTINGS.saveToFile();
                             }
                           });
  } else if (selectedIndex == 3) {
    SETTINGS.opdsFileFolder =
        (SETTINGS.opdsFileFolder + 1) % static_cast<uint8_t>(CalibreSettingsActivity::getFileFolderSetting().size());
    SETTINGS.saveToFile();
    requestUpdate();
  }
}

std::vector<StrId> CalibreSettingsActivity::getFileFolderSetting() {
  if (fileFolderSettingEnums.size() == 0) {
    fileFolderSettingEnums =
        (*std::find_if(getSettingsList().begin(), getSettingsList().end(), [](const SettingInfo& s) {
          return strcmp(s.key, "opdsFileFolder") == 0;
        })).enumValues;
  }
  return fileFolderSettingEnums;
}

void CalibreSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_OPDS_BROWSER));
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    tr(STR_CALIBRE_URL_HINT));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + metrics.tabBarHeight;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(MENU_ITEMS),
      static_cast<int>(selectedIndex), [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr,
      nullptr,
      [this](int index) {
        // Draw status for each setting
        if (index == 0) {
          return (strlen(SETTINGS.opdsServerUrl) > 0) ? std::string(SETTINGS.opdsServerUrl)
                                                      : std::string(tr(STR_NOT_SET));
        } else if (index == 1) {
          return (strlen(SETTINGS.opdsUsername) > 0) ? std::string(SETTINGS.opdsUsername)
                                                     : std::string(tr(STR_NOT_SET));
        } else if (index == 2) {
          return (strlen(SETTINGS.opdsPassword) > 0) ? std::string("******") : std::string(tr(STR_NOT_SET));
        } else if (index == 3) {
          const uint8_t value = SETTINGS.opdsFileFolder;
          return std::string(I18N.get(CalibreSettingsActivity::getFileFolderSetting()[value]));
        }
        return std::string(tr(STR_NOT_SET));
      },
      true);

  // Draw help text at bottom
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
